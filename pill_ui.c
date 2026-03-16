#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>

#define DEV_FILE_PATH   "/home/autopilldispense/Desktop/pill_ui.c"
#define SCHEDULE_FILE   ".pill_dispenser_schedule.txt"
#define ADMIN_LOG_PATH  "/home/autopilldispense/Desktop/admin.txt"
#define MAX_TIMES       10
#define MAX_FOCUS_WIDGETS 310

static const char *DAYS[7] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
static int motor_pins[16] = {17, 27, 22, 5, 6, 13, 19, 26, 18, 23, 24, 25, 12, 16, 20, 21};

static int keypad_cols[3] = {2, 3, 4};
static int keypad_rows[4] = {14, 15, 8, 7};

static char keypad_map[4][3] = {
    {'1','2','3'},
    {'4','5','6'},
    {'7','8','9'},
    {'*','0','#'}
};

typedef struct {
    int h;
    int m;
    int enabled;
} HM;

typedef struct {
    GtkWidget *window;
    GtkWidget *stack;

    GtkWidget *time_label_main;
    GtkWidget *today_sched_title;
    GtkWidget *today_sched_box;
    GtkWidget *today_empty_label;

    HM slot_times[7][2][MAX_TIMES];
    int visible_count[7][2];

    GtkWidget *sched_rows[7][2][MAX_TIMES];
    GtkWidget *sched_spin[7][2][MAX_TIMES][2];
    GtkWidget *sched_status_label;

    GtkWidget *time_label_timepage;
    GtkWidget *spin_year;
    GtkWidget *spin_month;
    GtkWidget *spin_day;
    GtkWidget *spin_hour;
    GtkWidget *spin_min;
    GtkWidget *spin_sec;
    GtkWidget *status_label_time;

    GtkWidget *wifi_time_label;
    GtkWidget *wifi_status_label;
    GtkWidget *wifi_current_label;
    GtkListStore *wifi_store;
    GtkWidget *wifi_tree;
    GtkWidget *wifi_selected_label;
    GtkWidget *wifi_pass_entry;
    char wifi_selected_ssid[256];

    GtkWidget *admin_status_label;
    GtkListStore *usb_store;
    GtkWidget *usb_tree;
    GtkWidget *usb_selected_label;
    char usb_selected_mount[512];

    GtkWidget *focused_widget[MAX_FOCUS_WIDGETS];
    int focus_count;
    int focus_index;

    GtkWidget *active_modal;

    char pending_digit;
    int pending_index;
    guint pending_commit_timer;
} App;

typedef struct {
    App *app;
    int day;
    int slot;
} DaySlotRef;

static void scan_focusable(App *app, GtkWidget *w);
static void populate_focus_widgets(App *app, GtkWidget *root);
static void refresh_focus_for_current_page(App *app);
static GtkWidget *get_current_focus_widget(App *app);
static void tree_move_selection(GtkWidget *tree, int dir);

static void trim(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len - 1])) s[--len] = 0;
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

static void now_string(char *out, size_t outsz) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(out, outsz, "%Y-%m-%d %H:%M:%S", lt);
}

static void now_stamp(char *out, size_t outsz) {
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    strftime(out, outsz, "%Y%m%d_%H%M%S", lt);
}

static void set_label_to_now(GtkWidget *label) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char buf[128];
    strftime(buf, sizeof(buf), "%a %b %d %Y  %I:%M:%S %p", lt);
    gtk_label_set_text(GTK_LABEL(label), buf);
}

static void ui_set_status(GtkWidget *label, const char *msg) {
    if (label) gtk_label_set_text(GTK_LABEL(label), msg ? msg : "");
}

static GtkWidget *get_focus_root(App *app) {
    if (app->active_modal && gtk_widget_get_visible(app->active_modal)) {
        return app->active_modal;
    }
    return gtk_stack_get_visible_child(GTK_STACK(app->stack));
}

static void show_page(App *app, const char *name) {
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), name);
    refresh_focus_for_current_page(app);
}

static void show_main(App *app)     { show_page(app, "main"); }
static void show_settings(App *app) { show_page(app, "settings"); }

static int get_today_index(void) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (!lt) return 0;
    return (lt->tm_wday + 6) % 7;
}

static GtkWidget *make_top_icon_button(const char *icon_name) {
    GtkWidget *btn = gtk_button_new();
    GtkWidget *img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);

    gtk_button_set_image(GTK_BUTTON(btn), img);
    gtk_button_set_always_show_image(GTK_BUTTON(btn), TRUE);
    gtk_widget_set_size_request(btn, 44, 44);
    gtk_widget_set_can_focus(btn, TRUE);
    gtk_button_set_focus_on_click(GTK_BUTTON(btn), TRUE);

    return btn;
}

static GtkWidget *make_spin(int min, int max, int step) {
    GtkAdjustment *adj = gtk_adjustment_new(min, min, max, step, 10, 0);
    GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    gtk_widget_set_can_focus(spin, TRUE);
    return spin;
}

static int time_compare(const void *a, const void *b) {
    const HM *ta = (const HM *)a;
    const HM *tb = (const HM *)b;

    if (!ta->enabled && !tb->enabled) return 0;
    if (!ta->enabled) return 1;
    if (!tb->enabled) return -1;

    int ma = ta->h * 60 + ta->m;
    int mb = tb->h * 60 + tb->m;
    return ma - mb;
}

static void sort_slot_times(App *app, int day, int slot) {
    qsort(app->slot_times[day][slot], MAX_TIMES, sizeof(HM), time_compare);
}

static void update_visible_count_from_data(App *app) {
    int d, s, i;
    for (d = 0; d < 7; d++) {
        for (s = 0; s < 2; s++) {
            int count = 0;
            for (i = 0; i < MAX_TIMES; i++) {
                if (app->slot_times[d][s][i].enabled) count++;
            }
            app->visible_count[d][s] = count;
        }
    }
}

static void set_default_schedules(App *app) {
    int d, s, i;
    for (d = 0; d < 7; d++) {
        for (s = 0; s < 2; s++) {
            app->visible_count[d][s] = 0;
            for (i = 0; i < MAX_TIMES; i++) {
                app->slot_times[d][s][i].h = 8;
                app->slot_times[d][s][i].m = 0;
                app->slot_times[d][s][i].enabled = 0;
            }
        }
    }
}

static void get_schedule_path(char *out, size_t outsz) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/pi";
    snprintf(out, outsz, "%s/%s", home, SCHEDULE_FILE);
}

static int save_schedules(App *app) {
    char path[512];
    FILE *f;
    int d, i;

    get_schedule_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f) return 0;

    for (d = 0; d < 7; d++) {
        fprintf(f, "%s ", DAYS[d]);

        for (i = 0; i < MAX_TIMES; i++) {
            HM *t = &app->slot_times[d][0][i];
            fprintf(f, "%d:%02d:%02d ", t->enabled, t->h, t->m);
        }

        fprintf(f, "| ");

        for (i = 0; i < MAX_TIMES; i++) {
            HM *t = &app->slot_times[d][1][i];
            fprintf(f, "%d:%02d:%02d ", t->enabled, t->h, t->m);
        }

        fprintf(f, "\n");
    }

    fclose(f);
    return 1;
}

static int load_new_schedule_format(App *app, const char *line_in) {
    char line[2048];
    char day[16];
    char *p;
    int d, s, i;

    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    trim(line);
    if (!line[0]) return 0;

    p = line;
    if (sscanf(p, "%15s", day) != 1) return 0;

    d = -1;
    for (i = 0; i < 7; i++) {
        if (strcmp(day, DAYS[i]) == 0) {
            d = i;
            break;
        }
    }
    if (d < 0) return 0;

    p = strchr(p, ' ');
    if (!p) return 0;
    p++;

    for (s = 0; s < 2; s++) {
        for (i = 0; i < MAX_TIMES; i++) {
            int en, h, m;

            while (*p == ' ') p++;
            if (s == 1 && *p == '|') p++;
            while (*p == ' ') p++;

            if (sscanf(p, "%d:%d:%d", &en, &h, &m) != 3) return 0;
            if (en < 0 || en > 1 || h < 0 || h > 23 || m < 0 || m > 59) return 0;

            app->slot_times[d][s][i].enabled = en;
            app->slot_times[d][s][i].h = h;
            app->slot_times[d][s][i].m = m;

            while (*p && *p != ' ') p++;
        }

        if (s == 0) {
            while (*p == ' ') p++;
            if (*p == '|') p++;
        }
    }

    return 1;
}

static int load_old_schedule_format(App *app, const char *line_in) {
    char day[16];
    int h1, m1, h2, m2;
    int d, i, s;

    if (sscanf(line_in, "%15s %d:%d %d:%d", day, &h1, &m1, &h2, &m2) != 5) {
        return 0;
    }

    d = -1;
    for (i = 0; i < 7; i++) {
        if (strcmp(day, DAYS[i]) == 0) {
            d = i;
            break;
        }
    }
    if (d < 0) return 0;

    for (s = 0; s < 2; s++) {
        for (i = 0; i < MAX_TIMES; i++) {
            app->slot_times[d][s][i].enabled = 0;
            app->slot_times[d][s][i].h = 8;
            app->slot_times[d][s][i].m = 0;
        }
    }

    if (h1 >= 0 && h1 <= 23 && m1 >= 0 && m1 <= 59) {
        app->slot_times[d][0][0].enabled = 1;
        app->slot_times[d][0][0].h = h1;
        app->slot_times[d][0][0].m = m1;
    }

    if (h2 >= 0 && h2 <= 23 && m2 >= 0 && m2 <= 59) {
        app->slot_times[d][1][0].enabled = 1;
        app->slot_times[d][1][0].h = h2;
        app->slot_times[d][1][0].m = m2;
    }

    return 1;
}

static int load_schedules(App *app) {
    char path[512];
    FILE *f;
    char line[2048];
    int seen = 0;
    int d, s;

    get_schedule_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0]) continue;

        if (strchr(line, '|') && strstr(line, "0:")) {
            if (load_new_schedule_format(app, line)) seen++;
        } else {
            if (load_old_schedule_format(app, line)) seen++;
        }
    }

    fclose(f);

    for (d = 0; d < 7; d++) {
        for (s = 0; s < 2; s++) {
            sort_slot_times(app, d, s);
        }
    }
    update_visible_count_from_data(app);
    return (seen > 0);
}

static void rebuild_today_schedule(App *app) {
    GList *children;
    GList *l;
    int today;
    char hdr[160];
    int s, i;
    int total_active = 0;

    children = gtk_container_get_children(GTK_CONTAINER(app->today_sched_box));
    for (l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    today = get_today_index();

    snprintf(hdr, sizeof(hdr),
             "<span size='x-large' weight='bold'>Today's Schedule - %s</span>",
             DAYS[today]);
    gtk_label_set_markup(GTK_LABEL(app->today_sched_title), hdr);

    for (s = 0; s < 2; s++) {
        for (i = 0; i < MAX_TIMES; i++) {
            if (app->slot_times[today][s][i].enabled) total_active++;
        }
    }

    if (total_active == 0) {
        gtk_widget_show(app->today_empty_label);
        gtk_box_pack_start(GTK_BOX(app->today_sched_box), app->today_empty_label, FALSE, FALSE, 0);
        gtk_widget_show(app->today_empty_label);
        gtk_widget_show_all(app->today_sched_box);
        return;
    }

    gtk_widget_hide(app->today_empty_label);

    for (s = 0; s < 2; s++) {
        GtkWidget *frame = gtk_frame_new(s == 0 ? "Pill Slot 1" : "Pill Slot 2");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        int slot_active = 0;

        gtk_container_set_border_width(GTK_CONTAINER(box), 8);
        gtk_container_add(GTK_CONTAINER(frame), box);

        for (i = 0; i < MAX_TIMES; i++) {
            if (!app->slot_times[today][s][i].enabled) continue;
            slot_active = 1;

            {
                char buf[32];
                GtkWidget *lbl;

                snprintf(buf, sizeof(buf), "%02d:%02d",
                         app->slot_times[today][s][i].h,
                         app->slot_times[today][s][i].m);

                lbl = gtk_label_new(buf);
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
                gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
            }
        }

        if (!slot_active) {
            GtkWidget *lbl = gtk_label_new("No time slots set");
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
        }

        gtk_box_pack_start(GTK_BOX(app->today_sched_box), frame, FALSE, FALSE, 0);
    }

    gtk_widget_show_all(app->today_sched_box);
}

static void admin_log_append_snapshot(App *app, const char *reason) {
    FILE *f = fopen(ADMIN_LOG_PATH, "a");
    int d, i;
    char ts[64];

    if (!f) return;

    now_string(ts, sizeof(ts));

    fprintf(f, "=== %s | %s ===\n", ts, reason ? reason : "Schedule update");
    for (d = 0; d < 7; d++) {
        fprintf(f, "%s  Slot1:", DAYS[d]);
        for (i = 0; i < MAX_TIMES; i++) {
            HM *t = &app->slot_times[d][0][i];
            if (t->enabled) fprintf(f, " %02d:%02d", t->h, t->m);
        }
        fprintf(f, "  |  Slot2:");
        for (i = 0; i < MAX_TIMES; i++) {
            HM *t = &app->slot_times[d][1][i];
            if (t->enabled) fprintf(f, " %02d:%02d", t->h, t->m);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\n");
    fclose(f);
}

static int admin_log_clear(void) {
    FILE *f = fopen(ADMIN_LOG_PATH, "w");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int pkexec_auth_ping(void) {
    int rc = system("pkexec /usr/bin/true >/dev/null 2>&1");
    if (rc == -1) return -1;
    return (rc == 0) ? 0 : 1;
}

static int run_keypad_dialog(App *app,
                             const char *title,
                             const char *primary,
                             const char *secondary,
                             const char *ok_text,
                             const char *cancel_text)
{
    GtkWidget *dlg;
    GtkWidget *area;
    GtkWidget *box;
    char *markup;
    GtkWidget *lbl1;
    GtkWidget *cancel_btn;
    GtkWidget *ok_btn;
    int resp;

    dlg = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title ? title : "Confirm");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(app->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_container_set_border_width(GTK_CONTAINER(dlg), 12);

    area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(area), box);

    markup = g_markup_printf_escaped(
        "<span size='large' weight='bold'>%s</span>",
        primary ? primary : ""
    );

    lbl1 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl1), markup);
    g_free(markup);

    gtk_label_set_line_wrap(GTK_LABEL(lbl1), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl1), 0.0f);
    gtk_box_pack_start(GTK_BOX(box), lbl1, FALSE, FALSE, 0);

    if (secondary && secondary[0]) {
        GtkWidget *lbl2 = gtk_label_new(secondary);
        gtk_label_set_line_wrap(GTK_LABEL(lbl2), TRUE);
        gtk_label_set_xalign(GTK_LABEL(lbl2), 0.0f);
        gtk_box_pack_start(GTK_BOX(box), lbl2, FALSE, FALSE, 0);
    }

    cancel_btn = gtk_dialog_add_button(
        GTK_DIALOG(dlg),
        cancel_text ? cancel_text : "Cancel",
        GTK_RESPONSE_CANCEL
    );

    ok_btn = gtk_dialog_add_button(
        GTK_DIALOG(dlg),
        ok_text ? ok_text : "OK",
        GTK_RESPONSE_OK
    );

    gtk_widget_set_can_focus(cancel_btn, TRUE);
    gtk_widget_set_can_focus(ok_btn, TRUE);
    gtk_button_set_focus_on_click(GTK_BUTTON(cancel_btn), TRUE);
    gtk_button_set_focus_on_click(GTK_BUTTON(ok_btn), TRUE);

    gtk_widget_show_all(dlg);

    app->active_modal = dlg;
    app->focus_count = 2;
    app->focus_index = 0;
    app->focused_widget[0] = cancel_btn;
    app->focused_widget[1] = ok_btn;

    gtk_widget_grab_focus(cancel_btn);
    while (gtk_events_pending()) gtk_main_iteration();

    resp = gtk_dialog_run(GTK_DIALOG(dlg));

    app->active_modal = NULL;
    gtk_widget_destroy(dlg);
    refresh_focus_for_current_page(app);

    return resp;
}

static void run_info_dialog(App *app,
                            const char *title,
                            const char *primary,
                            const char *secondary)
{
    run_keypad_dialog(app, title, primary, secondary, "OK", "Back");
}

static void on_power_clicked(GtkWidget *w, gpointer user_data) {
    App *app = (App *)user_data;
    int resp;

    (void)w;

    resp = run_keypad_dialog(
        app,
        "Shutdown",
        "Shut down the Raspberry Pi now?",
        "This will power off the device. You will need to turn it back on manually.",
        "Shut Down",
        "Cancel"
    );

    if (resp != GTK_RESPONSE_OK) return;

    while (1) {
        int auth = pkexec_auth_ping();
        if (auth == 0) break;
    }

    system("pkexec /usr/bin/systemctl poweroff");
}

static void on_dev_edit_clicked(GtkWidget *w, gpointer user_data) {
    App *app = (App *)user_data;
    char cmd[512];

    (void)w;

    gtk_window_unfullscreen(GTK_WINDOW(app->window));
    gtk_window_iconify(GTK_WINDOW(app->window));

    snprintf(cmd, sizeof(cmd), "geany %s &", DEV_FILE_PATH);
    system(cmd);
}

static void refresh_schedule_day_slot_visibility(App *app, int day, int slot) {
    int visible = app->visible_count[day][slot];
    int i;

    if (visible < 0) visible = 0;
    if (visible > MAX_TIMES) visible = MAX_TIMES;
    app->visible_count[day][slot] = visible;

    for (i = 0; i < MAX_TIMES; i++) {
        if (!app->sched_rows[day][slot][i]) continue;
        if (i < visible) gtk_widget_show(app->sched_rows[day][slot][i]);
        else gtk_widget_hide(app->sched_rows[day][slot][i]);
    }
}

static void refresh_all_schedule_visibility(App *app) {
    int d, s;
    for (d = 0; d < 7; d++) {
        for (s = 0; s < 2; s++) {
            refresh_schedule_day_slot_visibility(app, d, s);
        }
    }
}

static void on_slot_add_clicked(GtkWidget *w, gpointer user_data) {
    DaySlotRef *ref = (DaySlotRef *)user_data;
    App *app = ref->app;

    (void)w;

    if (app->visible_count[ref->day][ref->slot] < MAX_TIMES) {
        int idx = app->visible_count[ref->day][ref->slot];
        app->slot_times[ref->day][ref->slot][idx].enabled = 1;
        app->slot_times[ref->day][ref->slot][idx].h = 8;
        app->slot_times[ref->day][ref->slot][idx].m = 0;
        app->visible_count[ref->day][ref->slot]++;
    }

    refresh_schedule_day_slot_visibility(app, ref->day, ref->slot);
    refresh_focus_for_current_page(app);
}

static void on_slot_remove_clicked(GtkWidget *w, gpointer user_data) {
    DaySlotRef *ref = (DaySlotRef *)user_data;
    App *app = ref->app;

    (void)w;

    if (app->visible_count[ref->day][ref->slot] > 0) {
        int idx = app->visible_count[ref->day][ref->slot] - 1;
        app->slot_times[ref->day][ref->slot][idx].enabled = 0;
        app->slot_times[ref->day][ref->slot][idx].h = 8;
        app->slot_times[ref->day][ref->slot][idx].m = 0;
        app->visible_count[ref->day][ref->slot]--;
    }

    refresh_schedule_day_slot_visibility(app, ref->day, ref->slot);
    refresh_focus_for_current_page(app);
}

static void fill_schedule_spins_from_data(App *app) {
    int d, s, i;
    for (d = 0; d < 7; d++) {
        for (s = 0; s < 2; s++) {
            for (i = 0; i < MAX_TIMES; i++) {
                gtk_spin_button_set_value(
                    GTK_SPIN_BUTTON(app->sched_spin[d][s][i][0]),
                    app->slot_times[d][s][i].h
                );
                gtk_spin_button_set_value(
                    GTK_SPIN_BUTTON(app->sched_spin[d][s][i][1]),
                    app->slot_times[d][s][i].m
                );
            }
        }
    }
    refresh_all_schedule_visibility(app);
}

static void pull_schedule_data_from_spins(App *app) {
    int d, s, i;
    for (d = 0; d < 7; d++) {
        for (s = 0; s < 2; s++) {
            for (i = 0; i < MAX_TIMES; i++) {
                app->slot_times[d][s][i].h =
                    gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->sched_spin[d][s][i][0]));
                app->slot_times[d][s][i].m =
                    gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->sched_spin[d][s][i][1]));
                app->slot_times[d][s][i].enabled = (i < app->visible_count[d][s]) ? 1 : 0;
            }
            sort_slot_times(app, d, s);
        }
    }
    update_visible_count_from_data(app);
}

static void on_sched_save(GtkWidget *w, gpointer user_data) {
    App *app = (App *)user_data;

    (void)w;

    pull_schedule_data_from_spins(app);

    if (save_schedules(app)) {
        admin_log_append_snapshot(app, "Schedule saved");
        gtk_label_set_text(GTK_LABEL(app->sched_status_label), "Saved.");
        rebuild_today_schedule(app);
        fill_schedule_spins_from_data(app);
        refresh_focus_for_current_page(app);
    } else {
        gtk_label_set_text(GTK_LABEL(app->sched_status_label), "Failed to save schedule file.");
    }
}

static void show_schedules(App *app) {
    gtk_label_set_text(GTK_LABEL(app->sched_status_label), "");
    fill_schedule_spins_from_data(app);
    show_page(app, "schedules");
}

static void populate_spins_with_current_time(App *app) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_year),  lt->tm_year + 1900);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_month), lt->tm_mon + 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_day),   lt->tm_mday);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_hour),  lt->tm_hour);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_min),   lt->tm_min);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->spin_sec),   lt->tm_sec);
}

static void on_apply_time_date(GtkWidget *widget, gpointer user_data) {
    App *app = (App *)user_data;
    int year, month, day, hour, min, sec;
    char datetime[64];
    char cmd[256];
    int rc;

    (void)widget;

    year  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_year));
    month = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_month));
    day   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_day));
    hour  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_hour));
    min   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_min));
    sec   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_sec));

    if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        gtk_label_set_text(GTK_LABEL(app->status_label_time), "Invalid date/time values.");
        return;
    }

    snprintf(datetime, sizeof(datetime), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, min, sec);

    snprintf(cmd, sizeof(cmd), "pkexec /bin/date -s \"%s\"", datetime);

    rc = system(cmd);
    if (rc == 0) gtk_label_set_text(GTK_LABEL(app->status_label_time), "Time/date updated.");
    else gtk_label_set_text(GTK_LABEL(app->status_label_time), "Failed to set time/date (permission?).");
}

static void show_time_date(App *app) {
    populate_spins_with_current_time(app);
    gtk_label_set_text(GTK_LABEL(app->status_label_time), "");
    show_page(app, "time_date");
}

static gboolean command_exists(const char *cmd) {
    char buf[256];
    snprintf(buf, sizeof(buf), "command -v %s >/dev/null 2>&1", cmd);
    return system(buf) == 0;
}

static void wifi_update_current(App *app) {
    FILE *fp = popen("nmcli -t -f ACTIVE,SSID dev wifi 2>/dev/null | grep '^yes:' | head -n 1", "r");
    if (!fp) {
        gtk_label_set_text(GTK_LABEL(app->wifi_current_label), "Current: (unable to query)");
        return;
    }

    {
        char line[512] = {0};
        if (fgets(line, sizeof(line), fp)) {
            char *colon = strchr(line, ':');
            char *ssid = colon ? colon + 1 : line;
            char buf[512];

            trim(ssid);
            snprintf(buf, sizeof(buf), "Current: %s", ssid[0] ? ssid : "(none)");
            gtk_label_set_text(GTK_LABEL(app->wifi_current_label), buf);
        } else {
            gtk_label_set_text(GTK_LABEL(app->wifi_current_label), "Current: (none)");
        }
    }

    pclose(fp);
}

static void wifi_scan_and_fill(App *app) {
    FILE *fp;
    char line[1024];
    int added = 0;

    ui_set_status(app->wifi_status_label, "");

    if (!command_exists("nmcli")) {
        ui_set_status(app->wifi_status_label, "nmcli not found. Install NetworkManager.");
        return;
    }

    gtk_list_store_clear(app->wifi_store);

    fp = popen("nmcli -t -f SSID,SIGNAL,SECURITY dev wifi list --rescan yes 2>/dev/null", "r");
    if (!fp) {
        ui_set_status(app->wifi_status_label, "Failed to scan Wi-Fi.");
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *ssid;
        char *signal;
        char *security;
        GtkTreeIter iter;

        trim(line);
        if (!line[0]) continue;

        ssid = strtok(line, ":");
        signal = strtok(NULL, ":");
        security = strtok(NULL, ":");

        if (!ssid) continue;
        trim(ssid);
        if (!ssid[0]) continue;

        if (!signal) signal = (char *)"";
        if (!security) security = (char *)"";

        gtk_list_store_append(app->wifi_store, &iter);
        gtk_list_store_set(app->wifi_store, &iter, 0, ssid, 1, signal, 2, security, -1);
        added++;
    }

    pclose(fp);
    wifi_update_current(app);

    if (added == 0) ui_set_status(app->wifi_status_label, "No networks found.");
    else ui_set_status(app->wifi_status_label, "Select a network, enter password if needed, then Connect.");
}

static void on_wifi_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
    App *app = (App *)user_data;
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *ssid = NULL;
        gtk_tree_model_get(model, &iter, 0, &ssid, -1);
        if (ssid) {
            char buf[512];
            strncpy(app->wifi_selected_ssid, ssid, sizeof(app->wifi_selected_ssid) - 1);
            app->wifi_selected_ssid[sizeof(app->wifi_selected_ssid) - 1] = 0;

            snprintf(buf, sizeof(buf), "Selected: %s", app->wifi_selected_ssid);
            gtk_label_set_text(GTK_LABEL(app->wifi_selected_label), buf);

            g_free(ssid);
        }
    }
}

static void on_wifi_rescan(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    wifi_scan_and_fill((App *)user_data);
}

static void on_wifi_connect(GtkWidget *widget, gpointer user_data) {
    App *app = (App *)user_data;
    const char *pw;
    char cmd[1024];
    int rc;

    (void)widget;

    if (!app->wifi_selected_ssid[0]) {
        ui_set_status(app->wifi_status_label, "Pick a network first.");
        return;
    }
    if (!command_exists("nmcli")) {
        ui_set_status(app->wifi_status_label, "nmcli not found.");
        return;
    }

    if (app->pending_commit_timer) {
        g_source_remove(app->pending_commit_timer);
        app->pending_commit_timer = 0;
    }
    app->pending_digit = 0;
    app->pending_index = 0;

    pw = gtk_entry_get_text(GTK_ENTRY(app->wifi_pass_entry));
    if (strchr(app->wifi_selected_ssid, '"') || (pw && strchr(pw, '"'))) {
        ui_set_status(app->wifi_status_label, "SSID/password cannot contain quotes (\").");
        return;
    }

    if (pw && pw[0]) {
        snprintf(cmd, sizeof(cmd),
                 "pkexec nmcli dev wifi connect \"%s\" password \"%s\"",
                 app->wifi_selected_ssid, pw);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "pkexec nmcli dev wifi connect \"%s\"",
                 app->wifi_selected_ssid);
    }

    ui_set_status(app->wifi_status_label, "Connecting... (auth prompt may appear)");
    rc = system(cmd);

    if (rc == 0) ui_set_status(app->wifi_status_label, "Connected (or connection initiated).");
    else ui_set_status(app->wifi_status_label, "Failed to connect.");

    wifi_update_current(app);
}

static void show_wifi(App *app) {
    if (app->pending_commit_timer) {
        g_source_remove(app->pending_commit_timer);
        app->pending_commit_timer = 0;
    }
    app->pending_digit = 0;
    app->pending_index = 0;

    ui_set_status(app->wifi_status_label, "");
    gtk_entry_set_text(GTK_ENTRY(app->wifi_pass_entry), "");
    app->wifi_selected_ssid[0] = 0;
    gtk_label_set_text(GTK_LABEL(app->wifi_selected_label), "Selected: (none)");
    show_page(app, "wifi");
    wifi_scan_and_fill(app);
}

static void on_admin_reset(GtkWidget *w, gpointer user_data) {
    App *app = (App *)user_data;
    int resp;

    (void)w;

    resp = run_keypad_dialog(
        app,
        "Admin Reset",
        "Admin Reset?",
        "This will reset schedules back to default and erase all logs in admin.txt.",
        "Reset",
        "Cancel"
    );

    if (resp != GTK_RESPONSE_OK) return;

    set_default_schedules(app);
    save_schedules(app);
    admin_log_clear();
    admin_log_append_snapshot(app, "Admin reset (defaults applied)");

    rebuild_today_schedule(app);
    fill_schedule_spins_from_data(app);

    ui_set_status(app->admin_status_label, "Reset complete: defaults applied + admin.txt cleared.");
    refresh_focus_for_current_page(app);
}

static void usb_clear_store(App *app) {
    gtk_list_store_clear(app->usb_store);
    app->usb_selected_mount[0] = 0;
    gtk_label_set_text(GTK_LABEL(app->usb_selected_label), "Selected: (none)");
}

static void usb_scan_mounted(App *app) {
    FILE *fp;
    char line[1024];
    int added = 0;

    usb_clear_store(app);

    fp = popen("lsblk -o NAME,LABEL,MOUNTPOINT,RM,TRAN -nr 2>/dev/null", "r");
    if (!fp) {
        ui_set_status(app->admin_status_label, "Failed to scan drives.");
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        char name[64] = {0};
        char label[128] = {0};
        char mount[512] = {0};
        char rm[8] = {0};
        char tran[32] = {0};
        int n;
        int is_rm;
        int is_usb;

        trim(line);
        if (!line[0]) continue;

        n = sscanf(line, "%63s %127s %511s %7s %31s", name, label, mount, rm, tran);
        if (n < 4) continue;

        if (strcmp(mount, "-") == 0) continue;

        is_rm = (strcmp(rm, "1") == 0);
        is_usb = (n >= 5 && strcmp(tran, "usb") == 0);

        if (!(is_rm || is_usb)) continue;

        {
            GtkTreeIter iter;
            char display[900];

            gtk_list_store_append(app->usb_store, &iter);
            snprintf(display, sizeof(display), "%s  (%s)  ->  %s", name, label, mount);
            gtk_list_store_set(app->usb_store, &iter, 0, display, 1, mount, -1);
            added++;
        }
    }

    pclose(fp);

    if (added == 0) ui_set_status(app->admin_status_label, "No mounted USB/removable drives found. Plug in + wait for it to mount.");
    else ui_set_status(app->admin_status_label, "Select a destination drive, then press Export.");
}

static void on_usb_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
    App *app = (App *)user_data;
    GtkTreeModel *model = NULL;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gchar *mount = NULL;
        gtk_tree_model_get(model, &iter, 1, &mount, -1);
        if (mount) {
            char buf[700];
            strncpy(app->usb_selected_mount, mount, sizeof(app->usb_selected_mount) - 1);
            app->usb_selected_mount[sizeof(app->usb_selected_mount) - 1] = 0;

            snprintf(buf, sizeof(buf), "Selected: %s", app->usb_selected_mount);
            gtk_label_set_text(GTK_LABEL(app->usb_selected_label), buf);

            g_free(mount);
        }
    }
}

static void on_admin_extract_rescan(GtkWidget *w, gpointer user_data) {
    (void)w;
    usb_scan_mounted((App *)user_data);
}

static void on_admin_extract_export(GtkWidget *w, gpointer user_data) {
    App *app = (App *)user_data;
    FILE *src_touch;
    char stamp[64];
    char outpath[1024];
    FILE *src;
    FILE *dst;
    char buf[4096];
    size_t nread;
    int failed = 0;

    (void)w;

    if (!app->usb_selected_mount[0]) {
        ui_set_status(app->admin_status_label, "Pick a destination drive first.");
        return;
    }

    src_touch = fopen(ADMIN_LOG_PATH, "a");
    if (src_touch) fclose(src_touch);

    now_stamp(stamp, sizeof(stamp));
    snprintf(outpath, sizeof(outpath), "%s/pill_admin_export_%s.txt",
             app->usb_selected_mount, stamp);

    src = fopen(ADMIN_LOG_PATH, "rb");
    if (!src) {
        ui_set_status(app->admin_status_label, "Export failed: could not open admin.txt.");
        return;
    }

    dst = fopen(outpath, "wb");
    if (!dst) {
        fclose(src);
        ui_set_status(app->admin_status_label, "Export failed: could not write to selected drive.");
        return;
    }

    while ((nread = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, nread, dst) != nread) {
            failed = 1;
            break;
        }
    }

    if (ferror(src)) failed = 1;
    if (fclose(dst) != 0) failed = 1;
    fclose(src);

    if (failed) {
        remove(outpath);
        ui_set_status(app->admin_status_label, "Export failed while copying file.");
        return;
    }

    {
        char msg[1200];
        snprintf(msg, sizeof(msg), "Exported to: %s", outpath);
        ui_set_status(app->admin_status_label, msg);
    }
}

static void show_admin_extract(App *app) {
    ui_set_status(app->admin_status_label, "");
    usb_scan_mounted(app);
    show_page(app, "admin_extract");
}

static int gpio_read(int pin) {
    char cmd[64];
    char buf[128];
    FILE *fp;

    snprintf(cmd, sizeof(cmd), "raspi-gpio get %d", pin);

    fp = popen(cmd, "r");
    if (!fp) return -1;

    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return -1;
    }

    pclose(fp);
    return (strstr(buf, "level=0") != NULL) ? 0 : 1;
}

static void gpio_setup_outputs(void) {
    char cmd[128];
    int i;

    for (i = 0; i < 16; i++) {
        snprintf(cmd, sizeof(cmd), "raspi-gpio set %d op dl", motor_pins[i]);
        system(cmd);
    }

    for (i = 0; i < 3; i++) {
        snprintf(cmd, sizeof(cmd), "raspi-gpio set %d op dh", keypad_cols[i]);
        system(cmd);
    }

    for (i = 0; i < 4; i++) {
        snprintf(cmd, sizeof(cmd), "raspi-gpio set %d ip pu", keypad_rows[i]);
        system(cmd);
    }
}

static void gpio_write(int pin, int value) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "raspi-gpio set %d %s", pin, value ? "dh" : "dl");
    system(cmd);
}

static void dispense(int slot) {
    if (slot == 1) {
        gpio_write(motor_pins[4], 0);
        gpio_write(motor_pins[5], 0);
        gpio_write(motor_pins[6], 1);
        gpio_write(motor_pins[7], 1);
        usleep(1000000);

        gpio_write(motor_pins[6], 0);
        gpio_write(motor_pins[7], 0);

        gpio_write(motor_pins[3], 0);
        gpio_write(motor_pins[2], 0);
        gpio_write(motor_pins[0], 1);
        gpio_write(motor_pins[1], 1);
        usleep(1000000);

        gpio_write(motor_pins[0], 0);
        gpio_write(motor_pins[1], 0);

        gpio_write(motor_pins[3], 1);
        gpio_write(motor_pins[2], 1);
        gpio_write(motor_pins[0], 0);
        gpio_write(motor_pins[1], 0);
        usleep(1000000);

        gpio_write(motor_pins[3], 0);
        gpio_write(motor_pins[2], 0);

        gpio_write(motor_pins[4], 1);
        gpio_write(motor_pins[5], 1);
        gpio_write(motor_pins[6], 0);
        gpio_write(motor_pins[7], 0);
        usleep(1000000);

        gpio_write(motor_pins[4], 0);
        gpio_write(motor_pins[5], 0);

    } else if (slot == 2) {
        gpio_write(motor_pins[12], 0);
        gpio_write(motor_pins[13], 0);
        gpio_write(motor_pins[14], 1);
        gpio_write(motor_pins[15], 1);
        usleep(1000000);

        gpio_write(motor_pins[14], 0);
        gpio_write(motor_pins[15], 0);

        gpio_write(motor_pins[11], 0);
        gpio_write(motor_pins[10], 0);
        gpio_write(motor_pins[8], 1);
        gpio_write(motor_pins[9], 1);
        usleep(1000000);

        gpio_write(motor_pins[8], 0);
        gpio_write(motor_pins[9], 0);

        gpio_write(motor_pins[11], 1);
        gpio_write(motor_pins[10], 1);
        gpio_write(motor_pins[8], 0);
        gpio_write(motor_pins[9], 0);
        usleep(1000000);

        gpio_write(motor_pins[11], 0);
        gpio_write(motor_pins[10], 0);

        gpio_write(motor_pins[12], 1);
        gpio_write(motor_pins[13], 1);
        gpio_write(motor_pins[14], 0);
        gpio_write(motor_pins[15], 0);
        usleep(1000000);

        gpio_write(motor_pins[12], 0);
        gpio_write(motor_pins[13], 0);
    }
}

static GtkWidget *get_current_focus_widget(App *app) {
    if (!app || app->focus_count <= 0) return NULL;
    if (app->focus_index < 0 || app->focus_index >= app->focus_count) return NULL;
    return app->focused_widget[app->focus_index];
}

static const char *key_chars_for_digit(char key) {
    switch (key) {
        case '1': return ".@-_1";
        case '2': return "ABCabc2";
        case '3': return "DEFdef3";
        case '4': return "GHIghi4";
        case '5': return "JKLjkl5";
        case '6': return "MNOmno6";
        case '7': return "PQRSpqrs7";
        case '8': return "TUVtuv8";
        case '9': return "WXYZwxyz9";
        case '0': return " 0";
        default:  return NULL;
    }
}

static void wifi_entry_commit_pending(App *app) {
    if (app->pending_commit_timer) {
        g_source_remove(app->pending_commit_timer);
        app->pending_commit_timer = 0;
    }
    app->pending_digit = 0;
    app->pending_index = 0;
}

static gboolean commit_pending_char_cb(gpointer user_data) {
    App *app = (App *)user_data;
    app->pending_digit = 0;
    app->pending_index = 0;
    app->pending_commit_timer = 0;
    return FALSE;
}

static void wifi_entry_backspace(App *app) {
    GtkEditable *ed;
    gint pos, start, end;

    if (!GTK_IS_ENTRY(app->wifi_pass_entry)) return;

    ed = GTK_EDITABLE(app->wifi_pass_entry);
    pos = gtk_editable_get_position(ed);

    if (gtk_editable_get_selection_bounds(ed, &start, &end)) {
        gtk_editable_delete_text(ed, start, end);
        gtk_editable_set_position(ed, start);
        return;
    }

    if (pos > 0) {
        gtk_editable_delete_text(ed, pos - 1, pos);
        gtk_editable_set_position(ed, pos - 1);
    }
}

static void wifi_entry_replace_last_char(App *app, char ch) {
    GtkEditable *ed;
    gint pos;
    char s[2];

    if (!GTK_IS_ENTRY(app->wifi_pass_entry)) return;

    ed = GTK_EDITABLE(app->wifi_pass_entry);
    pos = gtk_editable_get_position(ed);
    s[0] = ch;
    s[1] = '\0';

    if (pos > 0) {
        gtk_editable_delete_text(ed, pos - 1, pos);
        pos = pos - 1;
    }

    gtk_editable_insert_text(ed, s, 1, &pos);
    gtk_editable_set_position(ed, pos);
}

static void wifi_entry_append_char(App *app, char ch) {
    GtkEditable *ed;
    gint pos;
    char s[2];

    if (!GTK_IS_ENTRY(app->wifi_pass_entry)) return;

    ed = GTK_EDITABLE(app->wifi_pass_entry);
    pos = gtk_editable_get_position(ed);
    s[0] = ch;
    s[1] = '\0';

    gtk_editable_insert_text(ed, s, 1, &pos);
    gtk_editable_set_position(ed, pos);
}

static void wifi_entry_handle_key(App *app, char key) {
    const char *chars;
    size_t len;
    char ch;

    if (!GTK_IS_ENTRY(app->wifi_pass_entry)) return;

    if (key == '*') {
        wifi_entry_commit_pending(app);
        wifi_entry_backspace(app);
        return;
    }

    if (key == '#') {
        wifi_entry_commit_pending(app);
        return;
    }

    chars = key_chars_for_digit(key);
    if (!chars) return;

    len = strlen(chars);
    if (len == 0) return;

    if (app->pending_digit == key) {
        app->pending_index = (app->pending_index + 1) % (int)len;
        ch = chars[app->pending_index];
        wifi_entry_replace_last_char(app, ch);

        if (app->pending_commit_timer) {
            g_source_remove(app->pending_commit_timer);
        }
        app->pending_commit_timer = g_timeout_add(1000, commit_pending_char_cb, app);
    } else {
        wifi_entry_commit_pending(app);

        app->pending_digit = key;
        app->pending_index = 0;
        ch = chars[0];
        wifi_entry_append_char(app, ch);
        app->pending_commit_timer = g_timeout_add(1000, commit_pending_char_cb, app);
    }
}

static char keypad_read(void) {
    int col, row;

    for (col = 0; col < 3; col++) {
        gpio_write(keypad_cols[col], 0);
        usleep(3000);

        for (row = 0; row < 4; row++) {
            int val = gpio_read(keypad_rows[row]);
            if (val == 0) {
                gpio_write(keypad_cols[col], 1);
                usleep(3000);
                return keypad_map[row][col];
            }
        }

        gpio_write(keypad_cols[col], 1);
        usleep(2000);
    }

    return 0;
}

static void tree_move_selection(GtkWidget *tree, int dir) {
    GtkTreeView *tv;
    GtkTreeSelection *sel;
    GtkTreeModel *model;
    GtkTreeIter iter;
    GtkTreePath *path = NULL;

    if (!GTK_IS_TREE_VIEW(tree)) return;

    tv = GTK_TREE_VIEW(tree);
    sel = gtk_tree_view_get_selection(tv);
    model = gtk_tree_view_get_model(tv);
    if (!model) return;

    if (!gtk_tree_selection_get_selected(sel, NULL, &iter)) {
        if (gtk_tree_model_get_iter_first(model, &iter)) {
            gtk_tree_selection_select_iter(sel, &iter);
            path = gtk_tree_model_get_path(model, &iter);
            gtk_tree_view_scroll_to_cell(tv, path, NULL, TRUE, 0.5f, 0.0f);
            gtk_tree_path_free(path);
        }
        return;
    }

    path = gtk_tree_model_get_path(model, &iter);
    if (!path) return;

    if (dir > 0) {
        gtk_tree_path_next(path);
        if (gtk_tree_model_get_iter(model, &iter, path)) {
            gtk_tree_selection_select_iter(sel, &iter);
            gtk_tree_view_scroll_to_cell(tv, path, NULL, TRUE, 0.5f, 0.0f);
        }
    } else if (dir < 0) {
        if (gtk_tree_path_prev(path)) {
            if (gtk_tree_model_get_iter(model, &iter, path)) {
                gtk_tree_selection_select_iter(sel, &iter);
                gtk_tree_view_scroll_to_cell(tv, path, NULL, TRUE, 0.5f, 0.0f);
            }
        }
    }

    gtk_tree_path_free(path);
}

static void ui_right(App *app) {
    if (!app || app->focus_count == 0) return;
    app->focus_index++;
    if (app->focus_index >= app->focus_count) app->focus_index = 0;
    gtk_widget_grab_focus(app->focused_widget[app->focus_index]);
}

static void ui_left(App *app) {
    if (!app || app->focus_count == 0) return;
    app->focus_index--;
    if (app->focus_index < 0) app->focus_index = app->focus_count - 1;
    gtk_widget_grab_focus(app->focused_widget[app->focus_index]);
}

static void ui_up(App *app) {
    GtkWidget *w = get_current_focus_widget(app);
    if (!w) return;

    if (app->active_modal && GTK_IS_BUTTON(w)) {
        ui_left(app);
        return;
    }

    if (GTK_IS_SPIN_BUTTON(w)) {
        gtk_spin_button_spin(GTK_SPIN_BUTTON(w), GTK_SPIN_STEP_FORWARD, 1.0);
        return;
    }

    if (GTK_IS_TREE_VIEW(w)) {
        tree_move_selection(w, -1);
        return;
    }

    ui_left(app);
}

static void ui_down(App *app) {
    GtkWidget *w = get_current_focus_widget(app);
    if (!w) return;

    if (app->active_modal && GTK_IS_BUTTON(w)) {
        ui_right(app);
        return;
    }

    if (GTK_IS_SPIN_BUTTON(w)) {
        gtk_spin_button_spin(GTK_SPIN_BUTTON(w), GTK_SPIN_STEP_BACKWARD, 1.0);
        return;
    }

    if (GTK_IS_TREE_VIEW(w)) {
        tree_move_selection(w, 1);
        return;
    }

    ui_right(app);
}

static void ui_select(App *app) {
    GtkWidget *w = get_current_focus_widget(app);
    if (!w) return;

    if (GTK_IS_BUTTON(w)) {
        g_signal_emit_by_name(w, "clicked");
        return;
    }

    if (GTK_IS_ENTRY(w) || GTK_IS_SPIN_BUTTON(w) || GTK_IS_TREE_VIEW(w)) {
        gtk_widget_grab_focus(w);
        return;
    }

    gtk_widget_activate(w);
}

static void ui_back(App *app) {
    const char *page;

    if (app->active_modal && GTK_IS_DIALOG(app->active_modal)) {
        gtk_dialog_response(GTK_DIALOG(app->active_modal), GTK_RESPONSE_CANCEL);
        return;
    }

    page = gtk_stack_get_visible_child_name(GTK_STACK(app->stack));
    if (!page) return;

    if (strcmp(page, "main") == 0) {
        return;
    } else if (strcmp(page, "settings") == 0) {
        show_main(app);
    } else {
        show_settings(app);
    }
}

static gboolean poll_keypad(gpointer user_data) {
    App *app = (App *)user_data;
    static char last_key = 0;
    GtkWidget *focused;
    char key = keypad_read();

    if (key == 0) {
        last_key = 0;
        return TRUE;
    }

    if (key == last_key) {
        return TRUE;
    }

    last_key = key;
    focused = get_current_focus_widget(app);

    if (focused == app->wifi_pass_entry) {
        if ((key >= '0' && key <= '9') || key == '*' || key == '#') {
            wifi_entry_handle_key(app, key);
            return TRUE;
        }
    }

    if (key == '2') ui_up(app);
    else if (key == '8') ui_down(app);
    else if (key == '4') ui_left(app);
    else if (key == '6') ui_right(app);
    else if (key == '#') ui_select(app);
    else if (key == '*') ui_back(app);

    return TRUE;
}

static void scan_focusable(App *app, GtkWidget *w) {
    if (!w) return;
    if (!gtk_widget_get_visible(w)) return;
    if (!gtk_widget_get_sensitive(w)) return;

    if ((GTK_IS_BUTTON(w) ||
         GTK_IS_SPIN_BUTTON(w) ||
         GTK_IS_ENTRY(w) ||
         GTK_IS_TREE_VIEW(w)) &&
        gtk_widget_get_can_focus(w)) {
        if (app->focus_count < MAX_FOCUS_WIDGETS) {
            app->focused_widget[app->focus_count++] = w;
        }
    }

    if (GTK_IS_CONTAINER(w)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(w));
        GList *l;
        for (l = children; l != NULL; l = l->next) {
            scan_focusable(app, GTK_WIDGET(l->data));
        }
        g_list_free(children);
    }
}

static void populate_focus_widgets(App *app, GtkWidget *root) {
    app->focus_count = 0;
    app->focus_index = 0;

    scan_focusable(app, root);

    if (app->focus_count > 0) {
        gtk_widget_grab_focus(app->focused_widget[0]);
    }
}

static void refresh_focus_for_current_page(App *app) {
    GtkWidget *root = get_focus_root(app);
    if (!root) return;
    populate_focus_widgets(app, root);
}

static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();

    gtk_css_provider_load_from_data(provider,
        "*:focus {"
        "  border: 3px solid #4A90E2;"
        "  background: #E6F2FF;"
        "}",
        -1,
        NULL);

    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    g_object_unref(provider);
}

static gboolean schedule_file_changed(time_t *last_mtime_out) {
    char path[512];
    struct stat st;

    get_schedule_path(path, sizeof(path));

    if (stat(path, &st) != 0) return FALSE;

    if (*last_mtime_out == 0) {
        *last_mtime_out = st.st_mtime;
        return FALSE;
    }
    if (st.st_mtime != *last_mtime_out) {
        *last_mtime_out = st.st_mtime;
        return TRUE;
    }
    return FALSE;
}

static gboolean tick_update_time(gpointer user_data) {
    App *app = (App *)user_data;
    static time_t last_mtime = 0;
    static int last_dispense_hour = -1;
    static int last_dispense_minute = -1;
    static int last_day = -1;
    time_t now;
    struct tm *t;
    int today;
    int i;

    if (app->time_label_main)     set_label_to_now(app->time_label_main);
    if (app->time_label_timepage) set_label_to_now(app->time_label_timepage);
    if (app->wifi_time_label)     set_label_to_now(app->wifi_time_label);

    if (schedule_file_changed(&last_mtime)) {
        load_schedules(app);
        rebuild_today_schedule(app);
        fill_schedule_spins_from_data(app);
        refresh_focus_for_current_page(app);
    }

    now = time(NULL);
    t = localtime(&now);
    if (!t) return TRUE;

    today = (t->tm_wday + 6) % 7;

    if (today != last_day) {
        rebuild_today_schedule(app);
        last_day = today;
    }

    if (t->tm_hour == last_dispense_hour && t->tm_min == last_dispense_minute) {
        return TRUE;
    }
    last_dispense_hour = t->tm_hour;
    last_dispense_minute = t->tm_min;

    for (i = 0; i < MAX_TIMES; i++) {
        HM s1 = app->slot_times[today][0][i];
        HM s2 = app->slot_times[today][1][i];

        if (s1.enabled && s1.h == t->tm_hour && s1.m == t->tm_min) {
            dispense(1);
        }
        if (s2.enabled && s2.h == t->tm_hour && s2.m == t->tm_min) {
            dispense(2);
        }
    }

    return TRUE;
}

static GtkWidget *build_main_page(App *app) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *title = gtk_label_new(NULL);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *btn_settings = make_top_icon_button("emblem-system");
    GtkWidget *btn_power = make_top_icon_button("system-shutdown");

    gtk_label_set_markup(GTK_LABEL(title), "<span size='xx-large' weight='bold'>Auto Pill Dispenser</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

    gtk_widget_set_hexpand(spacer, TRUE);

    g_signal_connect_swapped(btn_settings, "clicked", G_CALLBACK(show_settings), app);
    g_signal_connect(btn_power, "clicked", G_CALLBACK(on_power_clicked), app);

    gtk_box_pack_start(GTK_BOX(top), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), spacer, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(top), btn_settings, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), btn_power, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    app->time_label_main = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->time_label_main), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->time_label_main, FALSE, FALSE, 0);

    app->today_sched_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(app->today_sched_title), "<span size='x-large' weight='bold'>Today's Schedule</span>");
    gtk_label_set_xalign(GTK_LABEL(app->today_sched_title), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->today_sched_title, FALSE, FALSE, 0);

    app->today_sched_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(root), app->today_sched_box, FALSE, FALSE, 0);

    app->today_empty_label = gtk_label_new("No time slots set for today.");
    gtk_label_set_xalign(GTK_LABEL(app->today_empty_label), 0.0f);

    return root;
}

static GtkWidget *build_settings_page(App *app) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *back = gtk_button_new_with_label("← Back");
    GtkWidget *hdr = gtk_label_new(NULL);
    GtkWidget *grid = gtk_grid_new();
    const char *labels[6] = {"Schedules","Time & Date","Wi-Fi Config","Dev: Edit Code","Admin Extract","Admin Reset"};
    const char *icons[6]  = {"x-office-calendar","preferences-system-time","network-wireless","accessories-text-editor","document-send","edit-clear"};
    int i;

    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_main), app);

    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Settings</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(root), grid, TRUE, TRUE, 0);

    for (i = 0; i < 6; i++) {
        GtkWidget *btn = gtk_button_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget *img = gtk_image_new_from_icon_name(icons[i], GTK_ICON_SIZE_DIALOG);
        GtkWidget *lbl = gtk_label_new(labels[i]);

        gtk_container_set_border_width(GTK_CONTAINER(box), 14);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.5f);
        gtk_widget_set_can_focus(btn, TRUE);

        gtk_box_pack_start(GTK_BOX(box), img, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(btn), box);

        if (i == 0) g_signal_connect_swapped(btn, "clicked", G_CALLBACK(show_schedules), app);
        if (i == 1) g_signal_connect_swapped(btn, "clicked", G_CALLBACK(show_time_date), app);
        if (i == 2) g_signal_connect_swapped(btn, "clicked", G_CALLBACK(show_wifi), app);
        if (i == 3) g_signal_connect(btn, "clicked", G_CALLBACK(on_dev_edit_clicked), app);
        if (i == 4) g_signal_connect_swapped(btn, "clicked", G_CALLBACK(show_admin_extract), app);
        if (i == 5) g_signal_connect(btn, "clicked", G_CALLBACK(on_admin_reset), app);

        gtk_grid_attach(GTK_GRID(grid), btn, i % 2, i / 2, 1, 1);
    }

    return root;
}

static GtkWidget *build_slot_editor(App *app, int day, int slot) {
    GtkWidget *frame = gtk_frame_new(slot == 0 ? "Pill Slot 1" : "Pill Slot 2");
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *btn_add = gtk_button_new_with_label("+");
    GtkWidget *btn_sub = gtk_button_new_with_label("-");
    DaySlotRef *ref_add = g_malloc(sizeof(DaySlotRef));
    DaySlotRef *ref_sub = g_malloc(sizeof(DaySlotRef));
    int i;

    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_container_add(GTK_CONTAINER(frame), outer);

    gtk_box_pack_start(GTK_BOX(controls), gtk_label_new("Time slots:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), btn_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), btn_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), controls, FALSE, FALSE, 0);

    ref_add->app = app;
    ref_add->day = day;
    ref_add->slot = slot;

    ref_sub->app = app;
    ref_sub->day = day;
    ref_sub->slot = slot;

    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_slot_add_clicked), ref_add);
    g_signal_connect(btn_sub, "clicked", G_CALLBACK(on_slot_remove_clicked), ref_sub);

    for (i = 0; i < MAX_TIMES; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        char idxbuf[16];
        GtkWidget *idx;
        GtkWidget *h;
        GtkWidget *m;

        snprintf(idxbuf, sizeof(idxbuf), "%d.", i + 1);
        idx = gtk_label_new(idxbuf);
        gtk_label_set_xalign(GTK_LABEL(idx), 0.0f);

        h = make_spin(0, 23, 1);
        m = make_spin(0, 59, 1);

        app->sched_rows[day][slot][i] = row;
        app->sched_spin[day][slot][i][0] = h;
        app->sched_spin[day][slot][i][1] = m;

        gtk_box_pack_start(GTK_BOX(row), idx, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Hour"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), h, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), gtk_label_new("Min"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), m, FALSE, FALSE, 0);

        gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);
    }

    return frame;
}

static GtkWidget *build_schedules_page(App *app) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *back = gtk_button_new_with_label("← Back");
    GtkWidget *hdr = gtk_label_new(NULL);
    GtkWidget *save = gtk_button_new_with_label("Save");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *days_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    int d;

    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_settings), app);
    g_signal_connect(save, "clicked", G_CALLBACK(on_sched_save), app);

    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Schedules</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), save, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    gtk_container_set_border_width(GTK_CONTAINER(days_box), 4);
    gtk_container_add(GTK_CONTAINER(scroll), days_box);

    for (d = 0; d < 7; d++) {
        GtkWidget *day_frame = gtk_frame_new(DAYS[d]);
        GtkWidget *day_grid = gtk_grid_new();
        GtkWidget *slot1;
        GtkWidget *slot2;

        gtk_container_set_border_width(GTK_CONTAINER(day_grid), 10);
        gtk_grid_set_row_spacing(GTK_GRID(day_grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(day_grid), 12);
        gtk_container_add(GTK_CONTAINER(day_frame), day_grid);

        slot1 = build_slot_editor(app, d, 0);
        slot2 = build_slot_editor(app, d, 1);

        gtk_grid_attach(GTK_GRID(day_grid), slot1, 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(day_grid), slot2, 1, 0, 1, 1);

        gtk_box_pack_start(GTK_BOX(days_box), day_frame, FALSE, FALSE, 0);
    }

    app->sched_status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->sched_status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->sched_status_label, FALSE, FALSE, 0);

    return root;
}

static GtkWidget *build_time_date_page(App *app) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *back = gtk_button_new_with_label("← Back");
    GtkWidget *hdr = gtk_label_new(NULL);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *btn_now = gtk_button_new_with_label("Load Current Time");
    GtkWidget *btn_apply = gtk_button_new_with_label("Apply");

    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_settings), app);
    g_signal_connect_swapped(btn_now, "clicked", G_CALLBACK(populate_spins_with_current_time), app);
    g_signal_connect(btn_apply, "clicked", G_CALLBACK(on_apply_time_date), app);

    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Time & Date</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    app->time_label_timepage = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->time_label_timepage), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->time_label_timepage, FALSE, FALSE, 0);

    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(root), grid, FALSE, FALSE, 0);

    app->spin_year  = make_spin(2000, 2100, 1);
    app->spin_month = make_spin(1, 12, 1);
    app->spin_day   = make_spin(1, 31, 1);
    app->spin_hour  = make_spin(0, 23, 1);
    app->spin_min   = make_spin(0, 59, 1);
    app->spin_sec   = make_spin(0, 59, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Date (Y-M-D):"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->spin_year,  1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->spin_month, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->spin_day,   3, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("Time (H:M:S):"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->spin_hour, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->spin_min,  2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), app->spin_sec,  3, 1, 1, 1);

    gtk_box_pack_start(GTK_BOX(row), btn_now, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), btn_apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), row, FALSE, FALSE, 0);

    app->status_label_time = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->status_label_time), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->status_label_time, FALSE, FALSE, 0);

    return root;
}

static GtkWidget *build_wifi_page(App *app) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *back = gtk_button_new_with_label("← Back");
    GtkWidget *hdr = gtk_label_new(NULL);
    GtkWidget *rescan = gtk_button_new_with_label("Rescan");
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *c1;
    GtkTreeViewColumn *c2;
    GtkTreeViewColumn *c3;
    GtkTreeSelection *sel;
    GtkWidget *scroll;
    GtkWidget *pw_row;
    GtkWidget *pw_lbl;
    GtkWidget *connect;

    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_settings), app);
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_wifi_rescan), app);

    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Wi-Fi Config</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), rescan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    app->wifi_time_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->wifi_time_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->wifi_time_label, FALSE, FALSE, 0);

    app->wifi_current_label = gtk_label_new("Current: (unknown)");
    gtk_label_set_xalign(GTK_LABEL(app->wifi_current_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->wifi_current_label, FALSE, FALSE, 0);

    app->wifi_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    app->wifi_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->wifi_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->wifi_tree), TRUE);
    gtk_widget_set_can_focus(app->wifi_tree, TRUE);

    renderer = gtk_cell_renderer_text_new();
    c1 = gtk_tree_view_column_new_with_attributes("SSID", renderer, "text", 0, NULL);
    c2 = gtk_tree_view_column_new_with_attributes("Signal", renderer, "text", 1, NULL);
    c3 = gtk_tree_view_column_new_with_attributes("Security", renderer, "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->wifi_tree), c1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->wifi_tree), c2);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->wifi_tree), c3);

    sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->wifi_tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_wifi_selection_changed), app);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), app->wifi_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    app->wifi_selected_label = gtk_label_new("Selected: (none)");
    gtk_label_set_xalign(GTK_LABEL(app->wifi_selected_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->wifi_selected_label, FALSE, FALSE, 0);

    pw_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    pw_lbl = gtk_label_new("Password:");
    gtk_label_set_xalign(GTK_LABEL(pw_lbl), 0.0f);

    app->wifi_pass_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(app->wifi_pass_entry), TRUE);
    gtk_widget_set_hexpand(app->wifi_pass_entry, TRUE);
    gtk_widget_set_can_focus(app->wifi_pass_entry, TRUE);

    connect = gtk_button_new_with_label("Connect");
    g_signal_connect(connect, "clicked", G_CALLBACK(on_wifi_connect), app);

    gtk_box_pack_start(GTK_BOX(pw_row), pw_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pw_row), app->wifi_pass_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(pw_row), connect, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), pw_row, FALSE, FALSE, 0);

    app->wifi_status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->wifi_status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->wifi_status_label, FALSE, FALSE, 0);

    return root;
}

static GtkWidget *build_admin_extract_page(App *app) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *back = gtk_button_new_with_label("← Back");
    GtkWidget *hdr = gtk_label_new(NULL);
    GtkWidget *rescan = gtk_button_new_with_label("Rescan Drives");
    GtkCellRenderer *r;
    GtkTreeViewColumn *col;
    GtkTreeSelection *sel;
    GtkWidget *scroll;
    GtkWidget *btn_export;

    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_settings), app);
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_admin_extract_rescan), app);

    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Admin Extract</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), rescan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    app->usb_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    app->usb_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->usb_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->usb_tree), TRUE);
    gtk_widget_set_can_focus(app->usb_tree, TRUE);

    r = gtk_cell_renderer_text_new();
    col = gtk_tree_view_column_new_with_attributes("Mounted USB / Removable Drives", r, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->usb_tree), col);

    sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->usb_tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_usb_selection_changed), app);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), app->usb_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    app->usb_selected_label = gtk_label_new("Selected: (none)");
    gtk_label_set_xalign(GTK_LABEL(app->usb_selected_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->usb_selected_label, FALSE, FALSE, 0);

    btn_export = gtk_button_new_with_label("Export admin.txt to selected drive");
    g_signal_connect(btn_export, "clicked", G_CALLBACK(on_admin_extract_export), app);
    gtk_box_pack_start(GTK_BOX(root), btn_export, FALSE, FALSE, 0);

    app->admin_status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->admin_status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->admin_status_label, FALSE, FALSE, 0);

    return root;
}

int main(int argc, char **argv) {
    App app;
    memset(&app, 0, sizeof(app));

    app.wifi_selected_ssid[0] = 0;
    app.usb_selected_mount[0] = 0;
    app.focus_count = 0;
    app.focus_index = 0;
    app.active_modal = NULL;
    app.pending_digit = 0;
    app.pending_index = 0;
    app.pending_commit_timer = 0;

    gtk_init(&argc, &argv);
    load_css();

    set_default_schedules(&app);
    load_schedules(&app);

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Auto Pill Dispenser");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 980, 680);
    gtk_window_fullscreen(GTK_WINDOW(app.window));
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 16);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    app.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app.stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(app.stack), 160);
    gtk_container_add(GTK_CONTAINER(app.window), app.stack);

    gtk_stack_add_named(GTK_STACK(app.stack), build_main_page(&app), "main");
    gtk_stack_add_named(GTK_STACK(app.stack), build_settings_page(&app), "settings");
    gtk_stack_add_named(GTK_STACK(app.stack), build_schedules_page(&app), "schedules");
    gtk_stack_add_named(GTK_STACK(app.stack), build_time_date_page(&app), "time_date");
    gtk_stack_add_named(GTK_STACK(app.stack), build_wifi_page(&app), "wifi");
    gtk_stack_add_named(GTK_STACK(app.stack), build_admin_extract_page(&app), "admin_extract");

    show_main(&app);
    rebuild_today_schedule(&app);
    fill_schedule_spins_from_data(&app);

    gpio_setup_outputs();

    g_timeout_add(200, poll_keypad, &app);
    g_timeout_add(1000, tick_update_time, &app);
    tick_update_time(&app);

    gtk_widget_show_all(app.window);
    refresh_all_schedule_visibility(&app);
    refresh_focus_for_current_page(&app);

    gtk_main();
    return 0;
}
