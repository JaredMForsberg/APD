// pill_ui.c  (Basic KEYPAD VERSION)  — NO libgpiod (uses raspi-gpio)
// GTK3 UI + schedules + time/date + wifi + dev edit + power shutdown
// + Admin Reset (reset schedules + clear admin.txt)
// + Admin Extract (copy admin log to selected USB drive)
// + Admin Extract is gated by Pi user's password via pkexec (no separate admin password)
//
// UPDATES:
// functions added for basic Keypad functionallity, polling, focusing on specific widgets,
// ui navigation on main pages should work though it's is clunky and will need revisions
// use 4 as left and 6 as right, all widgets are treated as an array so keep clicking
//
// Compile:
//   gcc -std=gnu99 -O2 -Wall -Wextra /home/autopilldispense/Desktop/pill_ui.c -o /home/autopilldispense/Desktop/pill_ui $(pkg-config --cflags --libs gtk+-3.0)
//
// Run:
//   /home/autopilldispense/Desktop/pill_ui

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

    // Main page
    GtkWidget *time_label_main;
    GtkWidget *today_sched_title;
    GtkWidget *today_sched_box;
    GtkWidget *today_empty_label;

    // Schedule data [day][slot][time_index]
    HM slot_times[7][2][MAX_TIMES];
    int visible_count[7][2]; // how many rows are visible in scheduler UI per day/slot

    // Schedule UI
    GtkWidget *sched_rows[7][2][MAX_TIMES];
    GtkWidget *sched_spin[7][2][MAX_TIMES][2];
    GtkWidget *sched_status_label;

    // Time & Date page
    GtkWidget *time_label_timepage;
    GtkWidget *spin_year;
    GtkWidget *spin_month;
    GtkWidget *spin_day;
    GtkWidget *spin_hour;
    GtkWidget *spin_min;
    GtkWidget *spin_sec;
    GtkWidget *status_label_time;

    // Wi-Fi page
    GtkWidget *wifi_time_label;
    GtkWidget *wifi_status_label;
    GtkWidget *wifi_current_label;
    GtkListStore *wifi_store;
    GtkWidget *wifi_tree;
    GtkWidget *wifi_selected_label;
    GtkWidget *wifi_pass_entry;
    char wifi_selected_ssid[256];

    // Admin Extract page
    GtkWidget *admin_status_label;
    GtkListStore *usb_store;
    GtkWidget *usb_tree;
    GtkWidget *usb_selected_label;
    char usb_selected_mount[512];

    // Keypad Functionallity
    GtkWidget *focused_widget[MAX_FOCUS_WIDGETS];
    int focus_count;
    int focus_index;

    GtkWidget *current_grid;
    int focused_row;
    int focused_col;
} App;

typedef struct {
    App *app;
    int day;
    int slot;
} DaySlotRef;

// ------------------- helpers -------------------
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
    gtk_label_set_text(GTK_LABEL(label), msg ? msg : "");
}

static void show_page(App *app, const char *name) {
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), name);
}
static void show_main(App *app)     { show_page(app, "main"); }
static void show_settings(App *app) { show_page(app, "settings"); }

static int get_today_index(void) {
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (!lt) return 0;
    return (lt->tm_wday + 6) % 7; // Mon=0..Sun=6
}

static GtkWidget *make_cell_label(const char *text, gboolean header) {
    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    if (header) {
        char markup[256];
        snprintf(markup, sizeof(markup), "<b>%s</b>", text);
        gtk_label_set_markup(GTK_LABEL(lbl), markup);
    }
    return lbl;
}

static GtkWidget *make_top_icon_button(const char *icon_name) {
    GtkWidget *btn = gtk_button_new();
    GtkWidget *img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_button_set_image(GTK_BUTTON(btn), img);
    gtk_button_set_always_show_image(GTK_BUTTON(btn), TRUE);
    gtk_widget_set_size_request(btn, 44, 44);
    return btn;
}

static GtkWidget *make_spin(int min, int max, int step) {
    GtkAdjustment *adj = gtk_adjustment_new(min, min, max, step, 10, 0);
    GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    return spin;
}

static int schedule_entry_is_active(const HM *t) {
    return t && t->enabled;
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
    for (int d = 0; d < 7; d++) {
        for (int s = 0; s < 2; s++) {
            int count = 0;
            for (int i = 0; i < MAX_TIMES; i++) {
                if (app->slot_times[d][s][i].enabled) count++;
            }
            app->visible_count[d][s] = count; // allow 0
        }
    }
}

// ------------------- schedules persistence -------------------
static void set_default_schedules(App *app) {
    for (int d = 0; d < 7; d++) {
        for (int s = 0; s < 2; s++) {
            app->visible_count[d][s] = 0;
            for (int i = 0; i < MAX_TIMES; i++) {
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
    get_schedule_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return 0;

    for (int d = 0; d < 7; d++) {
        fprintf(f, "%s ", DAYS[d]);

        for (int i = 0; i < MAX_TIMES; i++) {
            HM *t = &app->slot_times[d][0][i];
            fprintf(f, "%d:%02d:%02d ", t->enabled, t->h, t->m);
        }

        fprintf(f, "| ");

        for (int i = 0; i < MAX_TIMES; i++) {
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
    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
    trim(line);
    if (!line[0]) return 0;

    char day[16];
    char *p = line;

    if (sscanf(p, "%15s", day) != 1) return 0;

    int d = -1;
    for (int i = 0; i < 7; i++) {
        if (strcmp(day, DAYS[i]) == 0) {
            d = i;
            break;
        }
    }
    if (d < 0) return 0;

    p = strchr(p, ' ');
    if (!p) return 0;
    p++;

    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < MAX_TIMES; i++) {
            while (*p == ' ') p++;
            if (s == 1 && *p == '|') p++;
            while (*p == ' ') p++;

            int en, h, m;
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

// legacy support: Mon 08:00 20:00
static int load_old_schedule_format(App *app, const char *line_in) {
    char day[16];
    int h1, m1, h2, m2;
    if (sscanf(line_in, "%15s %d:%d %d:%d", day, &h1, &m1, &h2, &m2) != 5) {
        return 0;
    }

    int d = -1;
    for (int i = 0; i < 7; i++) {
        if (strcmp(day, DAYS[i]) == 0) {
            d = i;
            break;
        }
    }
    if (d < 0) return 0;

    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < MAX_TIMES; i++) {
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
    get_schedule_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[2048];
    int seen = 0;

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

    for (int d = 0; d < 7; d++) {
        for (int s = 0; s < 2; s++) {
            sort_slot_times(app, d, s);
        }
    }
    update_visible_count_from_data(app);
    return (seen > 0);
}

// ------------------- main screen refresh -------------------
static void rebuild_today_schedule(App *app) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->today_sched_box));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    int today = get_today_index();

    char hdr[160];
    snprintf(hdr, sizeof(hdr),
             "<span size='x-large' weight='bold'>Today's Schedule - %s</span>",
             DAYS[today]);
    gtk_label_set_markup(GTK_LABEL(app->today_sched_title), hdr);

    int total_active = 0;
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < MAX_TIMES; i++) {
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

    for (int s = 0; s < 2; s++) {
        GtkWidget *frame = gtk_frame_new(s == 0 ? "Pill Slot 1" : "Pill Slot 2");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(box), 8);
        gtk_container_add(GTK_CONTAINER(frame), box);

        int slot_active = 0;
        for (int i = 0; i < MAX_TIMES; i++) {
            if (!app->slot_times[today][s][i].enabled) continue;
            slot_active = 1;

            char buf[32];
            snprintf(buf, sizeof(buf), "%02d:%02d",
                     app->slot_times[today][s][i].h,
                     app->slot_times[today][s][i].m);

            GtkWidget *lbl = gtk_label_new(buf);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
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

// ------------------- admin logging -------------------
static void admin_log_append_snapshot(App *app, const char *reason) {
    FILE *f = fopen(ADMIN_LOG_PATH, "a");
    if (!f) return;

    char ts[64];
    now_string(ts, sizeof(ts));

    fprintf(f, "=== %s | %s ===\n", ts, reason ? reason : "Schedule update");
    for (int d = 0; d < 7; d++) {
        fprintf(f, "%s  Slot1:", DAYS[d]);
        for (int i = 0; i < MAX_TIMES; i++) {
            HM *t = &app->slot_times[d][0][i];
            if (t->enabled) fprintf(f, " %02d:%02d", t->h, t->m);
        }
        fprintf(f, "  |  Slot2:");
        for (int i = 0; i < MAX_TIMES; i++) {
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

// ------------------- pkexec auth helper -------------------
static int pkexec_auth_ping(void) {
    int rc = system("pkexec /usr/bin/true >/dev/null 2>&1");
    if (rc == -1) return -1;
    return (rc == 0) ? 0 : 1;
}

// ------------------- power + dev -------------------
static void on_power_clicked(GtkWidget *w, gpointer user_data) {
    (void)w;
    App *app = (App *)user_data;

    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_OK_CANCEL,
        "Shut down the Raspberry Pi now?"
    );
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dlg),
        "This will power off the device. You will need to turn it back on manually."
    );

    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp != GTK_RESPONSE_OK) return;

    // Keep prompting for auth until it succeeds
    while (1) {
        int auth = pkexec_auth_ping();

        if (auth == 0) {
            break;
        }
    }

    // After successful auth, power off
    system("pkexec /usr/bin/systemctl poweroff");
}
static void on_dev_edit_clicked(GtkWidget *w, gpointer user_data) {
    (void)w;
    App *app = (App *)user_data;

    // Exit fullscreen if active
    gtk_window_unfullscreen(GTK_WINDOW(app->window));

    // Minimize the UI window
    gtk_window_iconify(GTK_WINDOW(app->window));

    // Open the source code editor
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "geany %s &", DEV_FILE_PATH);
    system(cmd);
}

// ------------------- schedule UI logic -------------------
static void refresh_schedule_day_slot_visibility(App *app, int day, int slot) {
    int visible = app->visible_count[day][slot];
    if (visible < 0) visible = 0;
    if (visible > MAX_TIMES) visible = MAX_TIMES;
    app->visible_count[day][slot] = visible;

    for (int i = 0; i < MAX_TIMES; i++) {
        if (!app->sched_rows[day][slot][i]) continue;

        if (i < visible) gtk_widget_show(app->sched_rows[day][slot][i]);
        else gtk_widget_hide(app->sched_rows[day][slot][i]);
    }
}

static void refresh_all_schedule_visibility(App *app) {
    for (int d = 0; d < 7; d++) {
        for (int s = 0; s < 2; s++) {
            refresh_schedule_day_slot_visibility(app, d, s);
        }
    }
}

static void on_slot_add_clicked(GtkWidget *w, gpointer user_data) {
    (void)w;
    DaySlotRef *ref = (DaySlotRef *)user_data;
    App *app = ref->app;

    if (app->visible_count[ref->day][ref->slot] < MAX_TIMES) {
        int idx = app->visible_count[ref->day][ref->slot];
        app->slot_times[ref->day][ref->slot][idx].enabled = 1;
        app->slot_times[ref->day][ref->slot][idx].h = 8;
        app->slot_times[ref->day][ref->slot][idx].m = 0;
        app->visible_count[ref->day][ref->slot]++;
    }

    refresh_schedule_day_slot_visibility(app, ref->day, ref->slot);
}

static void on_slot_remove_clicked(GtkWidget *w, gpointer user_data) {
    (void)w;
    DaySlotRef *ref = (DaySlotRef *)user_data;
    App *app = ref->app;

    if (app->visible_count[ref->day][ref->slot] > 0) {
        int idx = app->visible_count[ref->day][ref->slot] - 1;
        app->slot_times[ref->day][ref->slot][idx].enabled = 0;
        app->slot_times[ref->day][ref->slot][idx].h = 8;
        app->slot_times[ref->day][ref->slot][idx].m = 0;
        app->visible_count[ref->day][ref->slot]--;
    }

    refresh_schedule_day_slot_visibility(app, ref->day, ref->slot);
}

static void fill_schedule_spins_from_data(App *app) {
    for (int d = 0; d < 7; d++) {
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < MAX_TIMES; i++) {
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
    for (int d = 0; d < 7; d++) {
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < MAX_TIMES; i++) {
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
    (void)w;
    App *app = (App *)user_data;

    pull_schedule_data_from_spins(app);

    if (save_schedules(app)) {
        admin_log_append_snapshot(app, "Schedule saved");
        gtk_label_set_text(GTK_LABEL(app->sched_status_label), "Saved.");
        rebuild_today_schedule(app);
        fill_schedule_spins_from_data(app);
    } else {
        gtk_label_set_text(GTK_LABEL(app->sched_status_label), "Failed to save schedule file.");
    }
}

static void show_schedules(App *app) {
    gtk_label_set_text(GTK_LABEL(app->sched_status_label), "");
    fill_schedule_spins_from_data(app);
    show_page(app, "schedules");
}

// ------------------- time/date -------------------
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
    (void)widget;
    App *app = (App *)user_data;

    int year  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_year));
    int month = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_month));
    int day   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_day));
    int hour  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_hour));
    int min   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_min));
    int sec   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->spin_sec));

    if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) {
        gtk_label_set_text(GTK_LABEL(app->status_label_time), "Invalid date/time values.");
        return;
    }

    char datetime[64];
    snprintf(datetime, sizeof(datetime), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, min, sec);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pkexec /bin/date -s \"%s\"", datetime);

    int rc = system(cmd);
    if (rc == 0) gtk_label_set_text(GTK_LABEL(app->status_label_time), "Time/date updated.");
    else gtk_label_set_text(GTK_LABEL(app->status_label_time), "Failed to set time/date (permission?).");
}

static void show_time_date(App *app) {
    populate_spins_with_current_time(app);
    gtk_label_set_text(GTK_LABEL(app->status_label_time), "");
    show_page(app, "time_date");
}

// ------------------- wifi -------------------
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

    char line[512] = {0};
    if (fgets(line, sizeof(line), fp)) {
        char *colon = strchr(line, ':');
        char *ssid = colon ? colon + 1 : line;
        trim(ssid);

        char buf[512];
        snprintf(buf, sizeof(buf), "Current: %s", ssid[0] ? ssid : "(none)");
        gtk_label_set_text(GTK_LABEL(app->wifi_current_label), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(app->wifi_current_label), "Current: (none)");
    }
    pclose(fp);
}

static void wifi_scan_and_fill(App *app) {
    ui_set_status(app->wifi_status_label, "");

    if (!command_exists("nmcli")) {
        ui_set_status(app->wifi_status_label, "nmcli not found. Install NetworkManager.");
        return;
    }

    gtk_list_store_clear(app->wifi_store);

    FILE *fp = popen("nmcli -t -f SSID,SIGNAL,SECURITY dev wifi list --rescan yes 2>/dev/null", "r");
    if (!fp) {
        ui_set_status(app->wifi_status_label, "Failed to scan Wi-Fi.");
        return;
    }

    char line[1024];
    int added = 0;

    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (!line[0]) continue;

        char *ssid = strtok(line, ":");
        char *signal = strtok(NULL, ":");
        char *security = strtok(NULL, ":");

        if (!ssid) continue;
        trim(ssid);
        if (!ssid[0]) continue;

        if (!signal) signal = (char *)"";
        if (!security) security = (char *)"";

        GtkTreeIter iter;
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
            strncpy(app->wifi_selected_ssid, ssid, sizeof(app->wifi_selected_ssid) - 1);
            app->wifi_selected_ssid[sizeof(app->wifi_selected_ssid) - 1] = 0;

            char buf[512];
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
    (void)widget;
    App *app = (App *)user_data;

    if (!app->wifi_selected_ssid[0]) {
        ui_set_status(app->wifi_status_label, "Pick a network first.");
        return;
    }
    if (!command_exists("nmcli")) {
        ui_set_status(app->wifi_status_label, "nmcli not found.");
        return;
    }

    const char *pw = gtk_entry_get_text(GTK_ENTRY(app->wifi_pass_entry));
    if (strchr(app->wifi_selected_ssid, '"') || (pw && strchr(pw, '"'))) {
        ui_set_status(app->wifi_status_label, "SSID/password cannot contain quotes (\").");
        return;
    }

    char cmd[1024];
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
    int rc = system(cmd);

    if (rc == 0) ui_set_status(app->wifi_status_label, "Connected (or connection initiated).");
    else ui_set_status(app->wifi_status_label, "Failed to connect.");

    wifi_update_current(app);
}

static void show_wifi(App *app) {
    ui_set_status(app->wifi_status_label, "");
    gtk_entry_set_text(GTK_ENTRY(app->wifi_pass_entry), "");
    app->wifi_selected_ssid[0] = 0;
    gtk_label_set_text(GTK_LABEL(app->wifi_selected_label), "Selected: (none)");
    show_page(app, "wifi");
    wifi_scan_and_fill(app);
}

// ------------------- Admin: Reset & Extract -------------------
static void on_admin_reset(GtkWidget *w, gpointer user_data) {
    (void)w;
    App *app = (App *)user_data;

    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_WARNING,
        GTK_BUTTONS_OK_CANCEL,
        "Admin Reset?"
    );
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
        "This will reset schedules back to default and erase all logs in admin.txt.");

    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp != GTK_RESPONSE_OK) return;

    set_default_schedules(app);
    save_schedules(app);
    admin_log_clear();
    admin_log_append_snapshot(app, "Admin reset (defaults applied)");

    rebuild_today_schedule(app);
    fill_schedule_spins_from_data(app);

    ui_set_status(app->admin_status_label, "Reset complete: defaults applied + admin.txt cleared.");
}

static void usb_clear_store(App *app) {
    gtk_list_store_clear(app->usb_store);
    app->usb_selected_mount[0] = 0;
    gtk_label_set_text(GTK_LABEL(app->usb_selected_label), "Selected: (none)");
}

static void usb_scan_mounted(App *app) {
    usb_clear_store(app);

    FILE *fp = popen("lsblk -o NAME,LABEL,MOUNTPOINT,RM,TRAN -nr 2>/dev/null", "r");
    if (!fp) {
        ui_set_status(app->admin_status_label, "Failed to scan drives.");
        return;
    }

    char line[1024];
    int added = 0;

    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (!line[0]) continue;

        char name[64]={0}, label[128]={0}, mount[512]={0}, rm[8]={0}, tran[32]={0};
        int n = sscanf(line, "%63s %127s %511s %7s %31s", name, label, mount, rm, tran);
        if (n < 4) continue;

        if (strcmp(mount, "-") == 0) continue;

        int is_rm = (strcmp(rm, "1") == 0);
        int is_usb = (n >= 5 && strcmp(tran, "usb") == 0);

        if (!(is_rm || is_usb)) continue;

        GtkTreeIter iter;
        gtk_list_store_append(app->usb_store, &iter);

        char display[900];
        snprintf(display, sizeof(display), "%s  (%s)  ->  %s", name, label, mount);

        gtk_list_store_set(app->usb_store, &iter,
                           0, display,
                           1, mount,
                           -1);
        added++;
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
            strncpy(app->usb_selected_mount, mount, sizeof(app->usb_selected_mount) - 1);
            app->usb_selected_mount[sizeof(app->usb_selected_mount) - 1] = 0;

            char buf[700];
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
    (void)w;
    App *app = (App *)user_data;

    if (!app->usb_selected_mount[0]) {
        ui_set_status(app->admin_status_label, "Pick a destination drive first.");
        return;
    }

    int auth = pkexec_auth_ping();
    if (auth != 0) {
        ui_set_status(app->admin_status_label, "Export cancelled (auth required).");
        return;
    }

    FILE *f = fopen(ADMIN_LOG_PATH, "a");
    if (f) fclose(f);

    char stamp[64];
    now_stamp(stamp, sizeof(stamp));

    char outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s/pill_admin_export_%s.txt", app->usb_selected_mount, stamp);

    if (strchr(outpath, '"') || strchr(ADMIN_LOG_PATH, '"')) {
        ui_set_status(app->admin_status_label, "Export failed: invalid path (quotes not allowed).");
        return;
    }

    char cmd[1400];
    snprintf(cmd, sizeof(cmd),
             "pkexec /bin/sh -c \"cp '%s' '%s'\"",
             ADMIN_LOG_PATH, outpath);

    int rc = system(cmd);
    if (rc == 0) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "Exported to: %s", outpath);
        ui_set_status(app->admin_status_label, msg);
    } else {
        ui_set_status(app->admin_status_label, "Export failed. Drive may be read-only or not mounted.");
    }
}

static void show_admin_extract(App *app) {
    int auth = pkexec_auth_ping();
    if (auth != 0) return;

    ui_set_status(app->admin_status_label, "");
    usb_scan_mounted(app);
    show_page(app, "admin_extract");
}

// ------------------- GPIO via raspi-gpio -------------------
static void gpio_setup_outputs(void) {
    for (int i = 0; i < 16; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "raspi-gpio set %d op dl", motor_pins[i]); //default low
        system(cmd);
    }
    for (int i = 0; i < 3; i++) {
        snprintf(smd, sizeof(cmd), "raspi-gpio set %d op dh", keypad_cols[i]); //default high
        system(cmd);
    }
    for (int i = 0; i < 4; i++) {
        snprintf(smd, sizeof(cmd), "raspi-gpio set %d ip pu", keypad_rows[i]); //pull up on these pins
        system(cmd);
    }
}

static int gpio_read(int pin) {
    char cmd[64];
    char buf[128];

    snprintf(cmd, sizeof(cmd), "raspi-gpio get %d", pin);

    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;

    fgets(buf, sizeof(buf), fp);
    pclose(fp);

    return (strstr(buf, "level=0") != NULL) ? 0 : 1;
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

// ------------- Keypad specific Function -----------
char keypad_read() {
    for (int col = 0; col < 3; col++) {
        gpio_write(keypad_cols[col], 0);
        for (int row = 0; row < 4; row++) {
            if((gpio_read(keypad_rows[row])) == 0) {
                gpio_write(keypad_cols[col], 1); //might need to add 1 ms delays if it polls to fast.
                return keypad_map[row][col];
            }
        }
        gpio_write(keypad_cols[col], 1);
    }
    return 0; //no key pressed
}

static gboolean poll_keypad(gpointer user_data) {
    App *app = (App *)user_data;
    char key = keypad_read(); 

    if (key) {
        switch(key){
            case '4' : ui_left(app); break;
            case '6' : ui_right(app); break;
            case '5' : ui_down(app); break;
            case '2' : ui_up(app); break;
            case '#' : ui_select(app); break;
            case '*' : ui_back(app); break;
            default : break;
        }
    }
    return TRUE;
}

void ui_right(App *app) {
    if (app -> focus_count == 0) { //handle no widets case, shouldn't occur
        return;
    }
    app->focus_index++;
    
    if(app->focus_index >= app->focus_count) { //handle far right case
        app ->focus_index = 0;
    }
    gtk_widget_grab_focus(app->focus_widgets[app->focus_index]); //move the focus
}

void ui_left(App *app) {
    if (app -> focus_count == 0) { //handle no widets case, shouldn't occur
        return;
    }
    app->focus_index--;

     if(app->focus_index < 0) { //handle far left case
        app ->focus_index = app->focus_count -1 ;
    }
    gtk_widget_grab_focus(app=>focus_index); //move the focus
}

void ui_up(App *app) {
    ui_left(app); //this should change later but for now it's just a single array
}

void ui_down(App *app) {
    ui_right(app); //same thing here
}

void ui_select(App *app) {
    if (app->focus_count == 0) {
        return; //handle case where there was nothing to select
    }
    GtkWidget *w = app->focus_widget[app->focus_index];

    if(GTK_IS_BUTTON(w)) { //if it is a button, select it
        gtk_button_clicked(GTK_BUTTON(w));
    }
}

void ui_back(App *app) {
    show_setting(app); //I think this will have to change since it only goes to the outside menu right now
}

// -------------------focus for keypad --------------
void populate_focus_widgets(App *app, GtkWidget *root) {
    app->focus_count = 0;
    app->focus_index = 0;

    scan_focusable(app, root);

    if (app->focus_count > 0) {
        gtk_widget_grab_focus(app->focus_widgets[0]);
    }
}

//recursive function to get the widgets on a page
static void scan_focusable(App *app, GtkWidget *w) {
    if (!w) return;

    if (GTK_IS_BUTTON(w) ||
        GTK_IS_SPIN_BUTTON(w) ||
        GTK_IS_ENTRY(w) ||
        GTK_IS_TREE_VIEW(w)) {

        if (app->focus_count < MAX_FOCUS_WIDGETS) {
            app->focus_widgets[app->focus_count++] = w;
        }
    }

    if (GTK_IS_CONTAINER(w)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(w));

        for (GList *l = children; l != NULL; l = l->next) {
            scan_focusable(app, GTK_WIDGET(l->data));
        }

        g_list_free(children);
    }
}

// ------------------- timer tick -------------------
static gboolean schedule_file_changed(time_t *last_mtime_out) {
    char path[512];
    get_schedule_path(path, sizeof(path));

    struct stat st;
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

    if (app->time_label_main)     set_label_to_now(app->time_label_main);
    if (app->time_label_timepage) set_label_to_now(app->time_label_timepage);
    if (app->wifi_time_label)     set_label_to_now(app->wifi_time_label);

    static time_t last_mtime = 0;
    if (schedule_file_changed(&last_mtime)) {
        load_schedules(app);
        rebuild_today_schedule(app);
        fill_schedule_spins_from_data(app);
    }

    static int last_dispense_hour = -1;
    static int last_dispense_minute = -1;
    static int last_day = -1;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t) return TRUE;

    int today = (t->tm_wday + 6) % 7;

    if (today != last_day) {
        rebuild_today_schedule(app);
        last_day = today;
    }

    if (t->tm_hour == last_dispense_hour && t->tm_min == last_dispense_minute) {
        return TRUE;
    }
    last_dispense_hour = t->tm_hour;
    last_dispense_minute = t->tm_min;

    for (int i = 0; i < MAX_TIMES; i++) {
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

// ------------------- page builders -------------------
static GtkWidget *build_main_page(App *app) {
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<span size='xx-large' weight='bold'>Auto Pill Dispenser</span>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);

    GtkWidget *btn_settings = make_top_icon_button("emblem-system");
    GtkWidget *btn_power = make_top_icon_button("system-shutdown");
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
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_main), app);

    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Settings</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(root), grid, TRUE, TRUE, 0);

    const char *labels[6] = {"Schedules","Time & Date","Wi-Fi Config","Dev: Edit Code","Admin Extract","Admin Reset"};
    const char *icons[6]  = {"x-office-calendar","preferences-system-time","network-wireless","accessories-text-editor","document-send","edit-clear"};

    for (int i = 0; i < 6; i++) {
        GtkWidget *btn = gtk_button_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget *img = gtk_image_new_from_icon_name(icons[i], GTK_ICON_SIZE_DIALOG);
        GtkWidget *lbl = gtk_label_new(labels[i]);

        gtk_container_set_border_width(GTK_CONTAINER(box), 14);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.5f);

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
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_container_add(GTK_CONTAINER(frame), outer);

    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *btn_add = gtk_button_new_with_label("+");
    GtkWidget *btn_sub = gtk_button_new_with_label("-");

    gtk_box_pack_start(GTK_BOX(controls), gtk_label_new("Time slots:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), btn_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(controls), btn_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), controls, FALSE, FALSE, 0);

    DaySlotRef *ref_add = g_malloc(sizeof(DaySlotRef));
    ref_add->app = app;
    ref_add->day = day;
    ref_add->slot = slot;

    DaySlotRef *ref_sub = g_malloc(sizeof(DaySlotRef));
    ref_sub->app = app;
    ref_sub->day = day;
    ref_sub->slot = slot;

    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_slot_add_clicked), ref_add);
    g_signal_connect(btn_sub, "clicked", G_CALLBACK(on_slot_remove_clicked), ref_sub);

    for (int i = 0; i < MAX_TIMES; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        char idxbuf[16];
        snprintf(idxbuf, sizeof(idxbuf), "%d.", i + 1);
        GtkWidget *idx = gtk_label_new(idxbuf);
        gtk_label_set_xalign(GTK_LABEL(idx), 0.0f);

        GtkWidget *h = make_spin(0, 23, 1);
        GtkWidget *m = make_spin(0, 59, 1);

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
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_settings), app);

    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Schedules</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    GtkWidget *save = gtk_button_new_with_label("Save");
    g_signal_connect(save, "clicked", G_CALLBACK(on_sched_save), app);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), save, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    GtkWidget *days_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(days_box), 4);
    gtk_container_add(GTK_CONTAINER(scroll), days_box);

    for (int d = 0; d < 7; d++) {
        GtkWidget *day_frame = gtk_frame_new(DAYS[d]);
        GtkWidget *day_grid = gtk_grid_new();
        gtk_container_set_border_width(GTK_CONTAINER(day_grid), 10);
        gtk_grid_set_row_spacing(GTK_GRID(day_grid), 8);
        gtk_grid_set_column_spacing(GTK_GRID(day_grid), 12);
        gtk_container_add(GTK_CONTAINER(day_frame), day_grid);

        GtkWidget *slot1 = build_slot_editor(app, d, 0);
        GtkWidget *slot2 = build_slot_editor(app, d, 1);

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
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_settings), app);

    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Time & Date</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    app->time_label_timepage = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->time_label_timepage), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->time_label_timepage, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
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

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *btn_now = gtk_button_new_with_label("Load Current Time");
    g_signal_connect_swapped(btn_now, "clicked", G_CALLBACK(populate_spins_with_current_time), app);

    GtkWidget *btn_apply = gtk_button_new_with_label("Apply");
    g_signal_connect(btn_apply, "clicked", G_CALLBACK(on_apply_time_date), app);

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
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_settings), app);

    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Wi-Fi Config</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    GtkWidget *rescan = gtk_button_new_with_label("Rescan");
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_wifi_rescan), app);

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

    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c1 = gtk_tree_view_column_new_with_attributes("SSID", renderer, "text", 0, NULL);
    GtkTreeViewColumn *c2 = gtk_tree_view_column_new_with_attributes("Signal", renderer, "text", 1, NULL);
    GtkTreeViewColumn *c3 = gtk_tree_view_column_new_with_attributes("Security", renderer, "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->wifi_tree), c1);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->wifi_tree), c2);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->wifi_tree), c3);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->wifi_tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_wifi_selection_changed), app);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), app->wifi_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    app->wifi_selected_label = gtk_label_new("Selected: (none)");
    gtk_label_set_xalign(GTK_LABEL(app->wifi_selected_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->wifi_selected_label, FALSE, FALSE, 0);

    GtkWidget *pw_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *pw_lbl = gtk_label_new("Password:");
    gtk_label_set_xalign(GTK_LABEL(pw_lbl), 0.0f);

    app->wifi_pass_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(app->wifi_pass_entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(app->wifi_pass_entry), '*');
    gtk_widget_set_hexpand(app->wifi_pass_entry, TRUE);

    GtkWidget *connect = gtk_button_new_with_label("Connect");
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
    g_signal_connect_swapped(back, "clicked", G_CALLBACK(show_settings), app);

    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>Admin Extract</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);

    GtkWidget *rescan = gtk_button_new_with_label("Rescan Drives");
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_admin_extract_rescan), app);

    gtk_box_pack_start(GTK_BOX(top), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hdr, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), rescan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), top, FALSE, FALSE, 0);

    app->usb_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    app->usb_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->usb_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->usb_tree), TRUE);

    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Mounted USB / Removable Drives", r, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->usb_tree), col);

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(app->usb_tree));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_SINGLE);
    g_signal_connect(sel, "changed", G_CALLBACK(on_usb_selection_changed), app);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), app->usb_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    app->usb_selected_label = gtk_label_new("Selected: (none)");
    gtk_label_set_xalign(GTK_LABEL(app->usb_selected_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->usb_selected_label, FALSE, FALSE, 0);

    GtkWidget *btn_export = gtk_button_new_with_label("Export admin.txt to selected drive");
    g_signal_connect(btn_export, "clicked", G_CALLBACK(on_admin_extract_export), app);
    gtk_box_pack_start(GTK_BOX(root), btn_export, FALSE, FALSE, 0);

    app->admin_status_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->admin_status_label), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), app->admin_status_label, FALSE, FALSE, 0);

    return root;
}

//this will set up the border so that it's visible what is being highlighted, hopefully
static void load_css(void)
{
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

// ------------------- main -------------------
int main(int argc, char **argv) {
    App app;
    memset(&app, 0, sizeof(app));

    app.focus_count = 0;
    app.focus_index = 0;
    app.current_grid = NULL;
    app.focused_row = 0;
    app.focused_col = 0;
    
    app.wifi_selected_ssid[0] = 0;
    app.usb_selected_mount[0] = 0;

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

    populate_focus_widgets(&app, app.stack); 

    gpio_setup_outputs();
    g_timeout_add(50, poll_keypad, &app); //poll the keypad every 50 ms

    g_timeout_add(1000, tick_update_time, &app);
    tick_update_time(&app);

    gtk_widget_show_all(app.window);
    refresh_all_schedule_visibility(&app);
    gtk_main();
    return 0;
}
