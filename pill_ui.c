// pill_ui.c  (NO KEYPAD VERSION)
// GTK3 UI + schedules + time/date + wifi + dev edit + power shutdown
// + Admin Reset (reset schedules + clear admin.txt)
// + Admin Extract (copy admin log to selected USB drive)
//
// Compile:
//   gcc -std=gnu99 /home/autopilldispense/Desktop/pill_ui.c -o /home/autopilldispense/Desktop/pill_ui $(pkg-config --cflags --libs gtk+-3.0)
//
// Run:
//   /home/autopilldispense/Desktop/pill_ui

#include <gtk/gtk.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <gpiod.h> //For GPIO activation
#include <stdio.h> 

#define DEV_FILE_PATH "/home/autopilldispense/Desktop/pill_ui.c"
#define SCHEDULE_FILE ".pill_dispenser_schedule.txt"
#define ADMIN_LOG_PATH "/home/autopilldispense/Desktop/admin.txt"

static const char *DAYS[7] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};

static int motor_pins[16] = {17, 27, 22, 5, 6, 13, 19, 26, 18, 23, 24, 25, 12, 16, 20, 21};
struct gpiod_line *motor_lines[16];
static int reserved_pins [8] = {2, 3, 14, 15, 7, 8, 9, 10}; //for future use with keypad, could swap for 11 for one of these and maybe 4 

struct gpiod_chip *chip;

typedef struct { int h; int m; } HM;

typedef struct {
    GtkWidget *window;
    GtkWidget *stack;

    // Main page
    GtkWidget *time_label_main;
    GtkWidget *main_sched_labels[7][2];

    // Schedule data
    HM slot1[7];
    HM slot2[7];

    // Schedules page
    GtkWidget *sched_spin[7][2][2]; // [day][slot][hm] hm 0=hour 1=min
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
    GtkListStore *usb_store;     // columns: 0=display, 1=mountpoint
    GtkWidget *usb_tree;
    GtkWidget *usb_selected_label;
    char usb_selected_mount[512];

} App;

void dispense (int);

// ------------------- helpers -------------------
static void trim(char *s) {
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

static gboolean tick_update_time(gpointer user_data) {
    App *app = (App *)user_data;
    if (app->time_label_main) set_label_to_now(app->time_label_main);
    if (app->time_label_timepage) set_label_to_now(app->time_label_timepage);
    if (app->wifi_time_label) set_label_to_now(app->wifi_time_label);
    
    //#TODO: will need a way to make it not occur 60 times in a row. probably a way to just check the day first and then go to that slot?
    static int last_dispense_minute = -1;
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now); //gets current time
    
    if (t->tm_min == last_dispense_minute) {
        return TRUE; //return early if already checked this minute
    } else {
        last_dispense_minute = t->tm_min; //otherwise set the variable so that next time we wont check for a dispense
    }
     //does conversion so that monday is first day of the week, can be removed if we put sunday as first in the UI
     int today = (t->tm_wday +6) % 7;
    //Checks the day hour and time for dispense, first for slot 1 , then slot 2, will need to update once we do more than one pill per dispense.
    if (t->tm_hour == app->slot1[today].h && t->tm_min == app->slot1[today].m) {
        dispense(1);
    }
    
    if (t->tm_hour == app->slot2[today].h && t->tm_min == app->slot2[today].m) {
        dispense(2);
    }
    
    return TRUE;
}

static void show_page(App *app, const char *name) {
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), name);
}
static void show_main(App *app) { show_page(app, "main"); }
static void show_settings(App *app) { show_page(app, "settings"); }

// ------------------- UI helpers -------------------
static GtkWidget *make_cell_label(const char *text, gboolean header) {
    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.5f);
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

static void ui_set_status(GtkWidget *label, const char *msg) {
    gtk_label_set_text(GTK_LABEL(label), msg ? msg : "");
}

// ------------------- schedules persistence -------------------
static void set_default_schedules(App *app) {
    for (int i = 0; i < 7; i++) {
        app->slot1[i].h = 8;  app->slot1[i].m = 0;
        app->slot2[i].h = 20; app->slot2[i].m = 0;
    }
}

static void get_schedule_path(char *out, size_t outsz) {
    const char *home = getenv("HOME");
    if (!home) home = "/home/pi";
    snprintf(out, outsz, "%s/%s", home, SCHEDULE_FILE);
}

static int load_schedules(App *app) {
    char path[512];
    get_schedule_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[256];
    int seen = 0;

    while (fgets(line, sizeof(line), f)) {
        char day[16];
        int h1, m1, h2, m2;
        trim(line);
        if (!line[0]) continue;

        if (sscanf(line, "%15s %d:%d %d:%d", day, &h1, &m1, &h2, &m2) == 5) {
            for (int i = 0; i < 7; i++) {
                if (strcmp(day, DAYS[i]) == 0) {
                    if (h1>=0 && h1<=23 && m1>=0 && m1<=59 && h2>=0 && h2<=23 && m2>=0 && m2<=59) {
                        app->slot1[i].h = h1; app->slot1[i].m = m1;
                        app->slot2[i].h = h2; app->slot2[i].m = m2;
                        seen++;
                    }
                    break;
                }
            }
        }
    }

    fclose(f);
    return (seen > 0);
}

static int save_schedules(App *app) {
    char path[512];
    get_schedule_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return 0;

    for (int i = 0; i < 7; i++) {
        fprintf(f, "%s %02d:%02d %02d:%02d\n",
                DAYS[i],
                app->slot1[i].h, app->slot1[i].m,
                app->slot2[i].h, app->slot2[i].m);
    }
    fclose(f);
    return 1;
}

static void update_main_schedule_labels(App *app) {
    for (int i = 0; i < 7; i++) {
        char a[16], b[16];
        snprintf(a, sizeof(a), "%02d:%02d", app->slot1[i].h, app->slot1[i].m);
        snprintf(b, sizeof(b), "%02d:%02d", app->slot2[i].h, app->slot2[i].m);
        gtk_label_set_text(GTK_LABEL(app->main_sched_labels[i][0]), a);
        gtk_label_set_text(GTK_LABEL(app->main_sched_labels[i][1]), b);
    }
}

// ------------------- admin logging -------------------
static void admin_log_append_snapshot(App *app, const char *reason) {
    FILE *f = fopen(ADMIN_LOG_PATH, "a");
    if (!f) return;

    char ts[64];
    now_string(ts, sizeof(ts));

    fprintf(f, "=== %s | %s ===\n", ts, reason ? reason : "Schedule update");
    for (int i = 0; i < 7; i++) {
        fprintf(f, "%s  Slot1 %02d:%02d  Slot2 %02d:%02d\n",
                DAYS[i],
                app->slot1[i].h, app->slot1[i].m,
                app->slot2[i].h, app->slot2[i].m);
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

static int file_copy(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return 0; }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out);
            return 0;
        }
    }
    fclose(in);
    fclose(out);
    return 1;
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
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg),
        "This will power off the device. You will need to turn it back on manually.");

    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp == GTK_RESPONSE_OK) {
        system("pkexec /usr/sbin/shutdown -h now");
    }
}

static void on_dev_edit_clicked(GtkWidget *w, gpointer user_data) {
    (void)w;
    (void)user_data;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "geany %s &", DEV_FILE_PATH);
    system(cmd);
}

// ------------------- schedules page logic -------------------
static void fill_schedule_spins_from_data(App *app) {
    for (int d = 0; d < 7; d++) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->sched_spin[d][0][0]), app->slot1[d].h);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->sched_spin[d][0][1]), app->slot1[d].m);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->sched_spin[d][1][0]), app->slot2[d].h);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->sched_spin[d][1][1]), app->slot2[d].m);
    }
}

static void pull_schedule_data_from_spins(App *app) {
    for (int d = 0; d < 7; d++) {
        app->slot1[d].h = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->sched_spin[d][0][0]));
        app->slot1[d].m = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->sched_spin[d][0][1]));
        app->slot2[d].h = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->sched_spin[d][1][0]));
        app->slot2[d].m = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->sched_spin[d][1][1]));
    }
}

static void on_sched_save(GtkWidget *w, gpointer user_data) {
    (void)w;
    App *app = (App *)user_data;

    pull_schedule_data_from_spins(app);

    if (save_schedules(app)) {
        admin_log_append_snapshot(app, "Schedule saved");
        gtk_label_set_text(GTK_LABEL(app->sched_status_label), "Saved.");
        update_main_schedule_labels(app);
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
    admin_log_clear();                 // wipe logs
    admin_log_append_snapshot(app, "Admin reset (defaults applied)");

    update_main_schedule_labels(app);
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

    // We list mounted removable/USB drives via lsblk
    // NAME LABEL MOUNTPOINT RM TRAN
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

        // Parse: name label mount rm tran
        // Note: LABEL can be empty, so we do a more forgiving parse by splitting from end.
        // We'll token-split and then rebuild.
        char name[64]={0}, label[128]={0}, mount[512]={0}, rm[8]={0}, tran[32]={0};

        // easiest: read 5 tokens; if label is empty, lsblk still prints something, but can collapse.
        // We'll use sscanf with wide fields; if it fails, skip.
        if (sscanf(line, "%63s %127s %511s %7s %31s", name, label, mount, rm, tran) < 4) continue;

        // If mountpoint is "-", treat as not mounted
        if (strcmp(mount, "-") == 0) continue;

        // Must be removable or usb transport
        int is_rm = (strcmp(rm, "1") == 0);
        int is_usb = (strcmp(tran, "usb") == 0);

        if (!(is_rm || is_usb)) continue;

        GtkTreeIter iter;
        gtk_list_store_append(app->usb_store, &iter);

        char display[900];
        // If label looks like a mountpoint (due to weird tokenization), still show something useful
        snprintf(display, sizeof(display), "%s  (%s)  →  %s", name, label, mount);

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
    App *app = (App *)user_data;
    usb_scan_mounted(app);
}

static void on_admin_extract_export(GtkWidget *w, gpointer user_data) {
    (void)w;
    App *app = (App *)user_data;

    if (!app->usb_selected_mount[0]) {
        ui_set_status(app->admin_status_label, "Pick a destination drive first.");
        return;
    }

    // Ensure admin.txt exists (create if missing)
    FILE *f = fopen(ADMIN_LOG_PATH, "a");
    if (f) fclose(f);

    char stamp[64];
    now_stamp(stamp, sizeof(stamp));

    char outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s/pill_admin_export_%s.txt", app->usb_selected_mount, stamp);

    if (file_copy(ADMIN_LOG_PATH, outpath)) {
        char msg[1200];
        snprintf(msg, sizeof(msg), "Exported to: %s", outpath);
        ui_set_status(app->admin_status_label, msg);
    } else {
        ui_set_status(app->admin_status_label, "Export failed. Drive may be read-only or not mounted.");
    }
}

static void show_admin_extract(App *app) {
    ui_set_status(app->admin_status_label, "");
    usb_scan_mounted(app);
    show_page(app, "admin_extract");
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

    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<span size='x-large' weight='bold'>This Week’s Schedule</span>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0f);
    gtk_box_pack_start(GTK_BOX(root), hdr, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    gtk_grid_attach(GTK_GRID(grid), make_cell_label("Day", TRUE), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_cell_label("Pill Slot 1", TRUE), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_cell_label("Pill Slot 2", TRUE), 2, 0, 1, 1);

    for (int i = 0; i < 7; i++) {
        GtkWidget *lbl_day = make_cell_label(DAYS[i], FALSE);
        GtkWidget *lbl_s1  = make_cell_label("--:--", FALSE);
        GtkWidget *lbl_s2  = make_cell_label("--:--", FALSE);

        app->main_sched_labels[i][0] = lbl_s1;
        app->main_sched_labels[i][1] = lbl_s2;

        gtk_grid_attach(GTK_GRID(grid), lbl_day, 0, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), lbl_s1,  1, i + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), lbl_s2,  2, i + 1, 1, 1);
    }

    gtk_box_pack_start(GTK_BOX(root), grid, FALSE, FALSE, 0);
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

    // 6 icons now
    const char *ids[6]    = {"schedules","time_date","wifi","dev","admin_extract","admin_reset"};
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

        if (strcmp(ids[i], "schedules") == 0) g_signal_connect_swapped(btn, "clicked", G_CALLBACK(show_schedules), app);
        else if (strcmp(ids[i], "time_date") == 0) g_signal_connect_swapped(btn, "clicked", G_CALLBACK(show_time_date), app);
        else if (strcmp(ids[i], "wifi") == 0) g_signal_connect_swapped(btn, "clicked", G_CALLBACK(show_wifi), app);
        else if (strcmp(ids[i], "dev") == 0) g_signal_connect(btn, "clicked", G_CALLBACK(on_dev_edit_clicked), app);
        else if (strcmp(ids[i], "admin_extract") == 0) g_signal_connect_swapped(btn, "clicked", G_CALLBACK(show_admin_extract), app);
        else if (strcmp(ids[i], "admin_reset") == 0) g_signal_connect(btn, "clicked", G_CALLBACK(on_admin_reset), app);

        gtk_grid_attach(GTK_GRID(grid), btn, i % 2, i / 2, 1, 1);
    }

    return root;
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

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(root), grid, FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(grid), make_cell_label("Day", TRUE), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_cell_label("Slot 1 (HH:MM)", TRUE), 1, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), make_cell_label("Slot 2 (HH:MM)", TRUE), 3, 0, 2, 1);

    for (int d = 0; d < 7; d++) {
        GtkWidget *lbl_day = make_cell_label(DAYS[d], FALSE);

        GtkWidget *s1_h = make_spin(0, 23, 1);
        GtkWidget *s1_m = make_spin(0, 59, 1);
        GtkWidget *s2_h = make_spin(0, 23, 1);
        GtkWidget *s2_m = make_spin(0, 59, 1);

        app->sched_spin[d][0][0] = s1_h;
        app->sched_spin[d][0][1] = s1_m;
        app->sched_spin[d][1][0] = s2_h;
        app->sched_spin[d][1][1] = s2_m;

        gtk_grid_attach(GTK_GRID(grid), lbl_day, 0, d + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), s1_h,   1, d + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), s1_m,   2, d + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), s2_h,   3, d + 1, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), s2_m,   4, d + 1, 1, 1);
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

    // USB list
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

// ------------Dispenser Helper ---------------
//dispense logic, checks the slot, then actuates pull back, push out, pull back, push out sequentially
//could remove redundency but this allows for confirmation other pins are off
void dispense (int slot) {
    if (slot == 1) {
        gpiod_line_set_value(motor_lines[4], 0);
        gpiod_line_set_value(motor_lines[5], 0);
        gpiod_line_set_value(motor_lines[6], 1);
        gpiod_line_set_value(motor_lines[7], 1);
        
        usleep(1000000);//change to match delay needed for motor
        
        gpiod_line_set_value(motor_lines[6], 0);
        gpiod_line_set_value(motor_lines[7], 0);
        
        gpiod_line_set_value(motor_lines[3], 0);
        gpiod_line_set_value(motor_lines[2], 0);
        gpiod_line_set_value(motor_lines[0], 1);
        gpiod_line_set_value(motor_lines[1], 1);
        
        usleep(1000000);
        
        gpiod_line_set_value(motor_lines[0], 0);
        gpiod_line_set_value(motor_lines[1], 0);
        
        gpiod_line_set_value(motor_lines[3], 1);
        gpiod_line_set_value(motor_lines[2], 1);
        gpiod_line_set_value(motor_lines[0], 0);
        gpiod_line_set_value(motor_lines[1], 0);
        
        usleep(1000000);
        
        gpiod_line_set_value(motor_lines[3], 0);
        gpiod_line_set_value(motor_lines[2], 0);
        
        gpiod_line_set_value(motor_lines[4], 1);
        gpiod_line_set_value(motor_lines[5], 1);
        gpiod_line_set_value(motor_lines[6], 0);
        gpiod_line_set_value(motor_lines[7], 0);
        
        usleep(1000000);
        
        gpiod_line_set_value(motor_lines[4], 0);
        gpiod_line_set_value(motor_lines[5], 0);
        
    }else if (slot == 2) {
        
        gpiod_line_set_value(motor_lines[12], 0);
        gpiod_line_set_value(motor_lines[13], 0);
        gpiod_line_set_value(motor_lines[14], 1);
        gpiod_line_set_value(motor_lines[15], 1);
        
        usleep(1000000);//change to match delay needed for motor
        
        gpiod_line_set_value(motor_lines[14], 0);
        gpiod_line_set_value(motor_lines[15], 0);
        
        gpiod_line_set_value(motor_lines[11], 0);
        gpiod_line_set_value(motor_lines[10], 0);
        gpiod_line_set_value(motor_lines[8], 1);
        gpiod_line_set_value(motor_lines[9], 1);
        
        usleep(1000000);
        
        gpiod_line_set_value(motor_lines[8], 0);
        gpiod_line_set_value(motor_lines[9], 0);
        
        gpiod_line_set_value(motor_lines[11], 1);
        gpiod_line_set_value(motor_lines[10], 1);
        gpiod_line_set_value(motor_lines[8], 0);
        gpiod_line_set_value(motor_lines[9], 0);
        
        usleep(1000000);
        
        gpiod_line_set_value(motor_lines[11], 0);
        gpiod_line_set_value(motor_lines[10], 0);
        
        gpiod_line_set_value(motor_lines[12], 1);
        gpiod_line_set_value(motor_lines[13], 1);
        gpiod_line_set_value(motor_lines[14], 0);
        gpiod_line_set_value(motor_lines[15], 0);        
        
        usleep(1000000);
        
        gpiod_line_set_value(motor_lines[12], 0);
        gpiod_line_set_value(motor_lines[13], 0);
        
    }

}

// ------------------- MAIN -------------------
int main(int argc, char **argv) {
    App app;
    memset(&app, 0, sizeof(app));
    app.wifi_selected_ssid[0] = 0;
    app.usb_selected_mount[0] = 0;

    gtk_init(&argc, &argv);

    set_default_schedules(&app);
    load_schedules(&app);

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "Auto Pill Dispenser");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 920, 560);
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
    update_main_schedule_labels(&app);

    chip = gpiod_chip_open("/dev/gpiochip0");
    if(!chip) {
        return 1; //failure to open GPIO chip, cannot continue
    }
    for (int i = 0; i<16; i++) {
        motor_lines[i] = gpiod_chip_get_line(chip, motor_pins[i]);
        gpiod_line_request_output(motor_lines[i], "Pill-Dispenser", 0);
        }

    //dispense(1); //Adding for a test, delete this
    system("curl -s http://192.168.1.50/trigger > /dev/null"); //Also for testing probably should use something that can be retried if it fails for a better implementation

    g_timeout_add(1000, tick_update_time, &app);
    tick_update_time(&app);

    gtk_widget_show_all(app.window);
    gtk_main();
    return 0;
}
