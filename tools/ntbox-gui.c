/*
 * NT-Box Control Panel - Native GTK3 C implementation
 * Full control panel for NT-Box
 */

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <json-c/json.h>

#define ROOT_DIR "/home/imadix/Downloads/Niga"
#define NTSHELL ROOT_DIR "/tools/ntshell/ntshell"
#define NTBOX_KO ROOT_DIR "/ntbox.ko"
#define CONFIG_DIR g_build_path(G_DIR_SEPARATOR_S, g_get_home_dir(), ".config", "ntbox", NULL)
#define SHORTCUTS_FILE g_build_path(G_DIR_SEPARATOR_S, CONFIG_DIR, "shortcuts.json", NULL)

static GtkWidget *log_view;
static GtkTextBuffer *log_buffer;

static void log_append(const gchar *msg) {
    if (!log_buffer || !msg) return;
    GtkTextIter iter;
    gtk_text_buffer_get_end_iter(log_buffer, &iter);
    gtk_text_buffer_insert(log_buffer, &iter, msg, -1);
    gtk_text_buffer_insert(log_buffer, &iter, "\n", -1);
}

static gboolean check_module_loaded(void) {
    FILE *f = fopen("/proc/modules", "r");
    if (!f) return FALSE;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "ntbox")) {
            fclose(f);
            return TRUE;
        }
    }
    fclose(f);
    return FALSE;
}

static int run_sudo_command(const char *cmd[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execvp("sudo", (char **)cmd);
        exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void load_kernel_module_if_needed(void) {
    if (check_module_loaded()) {
        log_append("ntbox.ko: already loaded");
        return;
    }
    if (access(NTBOX_KO, F_OK) != 0) {
        log_append("ntbox.ko not found in project root");
        return;
    }
    log_append("ntbox.ko not loaded, attempting to load with sudo insmod...");
    const char *cmd[] = {"sudo", "insmod", NTBOX_KO, NULL};
    int rc = run_sudo_command(cmd);
    if (rc == 0) {
        log_append("ntbox.ko loaded successfully");
    } else {
        log_append("Failed to load ntbox.ko (may need password or permissions)");
    }
}

static void run_ntshell_command(const gchar *cmd) {
    gchar *argv[] = {(gchar *)NTSHELL, (gchar *)cmd, NULL};
    gchar *out = NULL, *err = NULL;
    gint status = 0;
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &status, NULL)) {
        if (out && strlen(out) > 0) log_append(out);
        if (err && strlen(err) > 0) log_append(err);
        g_free(out); g_free(err);
    }
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "NT-Box Control Panel");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 700);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_add(GTK_CONTAINER(window), vbox);
    gtk_container_set_border_width(GTK_CONTAINER(window), 6);
    
    // Notebook
    GtkWidget *notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);
    
    // Log view
    GtkWidget *log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(log_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_view), TRUE);
    log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    gtk_container_add(GTK_CONTAINER(log_scroll), log_view);
    gtk_box_pack_start(GTK_BOX(vbox), log_scroll, FALSE, TRUE, 0);
    gtk_widget_set_size_request(log_scroll, -1, 150);
    
    // Tab 1: Kernel & Control
    GtkWidget *tab1 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab1), 10);
    GtkWidget *lbl1 = gtk_label_new("Kernel Module & System Control");
    gtk_box_pack_start(GTK_BOX(tab1), lbl1, FALSE, FALSE, 0);
    GtkWidget *btn_load = gtk_button_new_with_label("Check & Load ntbox.ko");
    g_signal_connect_swapped(btn_load, "clicked", G_CALLBACK(load_kernel_module_if_needed), NULL);
    gtk_box_pack_start(GTK_BOX(tab1), btn_load, FALSE, FALSE, 0);
    GtkWidget *btn_selftest = gtk_button_new_with_label("Run Selftest");
    g_signal_connect_swapped(btn_selftest, "clicked", G_CALLBACK(run_ntshell_command), "selftest");
    gtk_box_pack_start(GTK_BOX(tab1), btn_selftest, FALSE, FALSE, 0);
    GtkWidget *btn_partitions = gtk_button_new_with_label("List Partitions");
    g_signal_connect_swapped(btn_partitions, "clicked", G_CALLBACK(run_ntshell_command), "partitions");
    gtk_box_pack_start(GTK_BOX(tab1), btn_partitions, FALSE, FALSE, 0);
    GtkWidget *btn_stats = gtk_button_new_with_label("Show Stats");
    g_signal_connect_swapped(btn_stats, "clicked", G_CALLBACK(run_ntshell_command), "stats");
    gtk_box_pack_start(GTK_BOX(tab1), btn_stats, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab1, gtk_label_new("Control"));
    
    // Tab 2: GPU
    GtkWidget *tab2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab2), 10);
    GtkWidget *lbl2 = gtk_label_new("GPU Driver Selection & Installation");
    gtk_box_pack_start(GTK_BOX(tab2), lbl2, FALSE, FALSE, 0);
    GtkWidget *btn_gpu_show = gtk_button_new_with_label("Show Current GPU Driver");
    g_signal_connect_swapped(btn_gpu_show, "clicked", G_CALLBACK(run_ntshell_command), "gpu");
    gtk_box_pack_start(GTK_BOX(tab2), btn_gpu_show, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab2, gtk_label_new("GPU"));
    
    // Tab 3: Runtimes
    GtkWidget *tab3 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab3), 10);
    GtkWidget *lbl3 = gtk_label_new("Runtime Installers (Java, OpenJDK, Python, WinRAR, Games)");
    gtk_box_pack_start(GTK_BOX(tab3), lbl3, FALSE, FALSE, 0);
    const char *runtimes[] = {"Java", "OpenJDK", "Python", "WinRAR", "Steam", "Games", NULL};
    for (int i = 0; runtimes[i]; i++) {
        gchar *txt = g_strdup_printf("Install %s", runtimes[i]);
        GtkWidget *btn = gtk_button_new_with_label(txt);
        g_free(txt);
        gtk_box_pack_start(GTK_BOX(tab3), btn, FALSE, FALSE, 0);
    }
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab3, gtk_label_new("Runtimes"));
    
    // Tab 4: Run EXE
    GtkWidget *tab4 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab4), 10);
    GtkWidget *lbl4 = gtk_label_new("Run Arbitrary .EXE Files & Games");
    gtk_box_pack_start(GTK_BOX(tab4), lbl4, FALSE, FALSE, 0);
    GtkWidget *btn_runexe = gtk_button_new_with_label("Run EXE via runexe");
    g_signal_connect_swapped(btn_runexe, "clicked", G_CALLBACK(run_ntshell_command), "runexe");
    gtk_box_pack_start(GTK_BOX(tab4), btn_runexe, FALSE, FALSE, 0);
    GtkWidget *btn_runuser = gtk_button_new_with_label("Run User EXE");
    g_signal_connect_swapped(btn_runuser, "clicked", G_CALLBACK(run_ntshell_command), "runuser");
    gtk_box_pack_start(GTK_BOX(tab4), btn_runuser, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab4, gtk_label_new("Run EXE"));
    
    // Tab 5: Partitions
    GtkWidget *tab5 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab5), 10);
    GtkWidget *lbl5 = gtk_label_new("Partition & Drive Access Management");
    gtk_box_pack_start(GTK_BOX(tab5), lbl5, FALSE, FALSE, 0);
    GtkWidget *btn_attach = gtk_button_new_with_label("Attach Partition/Drive");
    gtk_box_pack_start(GTK_BOX(tab5), btn_attach, FALSE, FALSE, 0);
    GtkWidget *btn_detach = gtk_button_new_with_label("Detach Drive");
    gtk_box_pack_start(GTK_BOX(tab5), btn_detach, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab5, gtk_label_new("Partitions"));
    
    // Tab 6: Shortcuts
    GtkWidget *tab6 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab6), 10);
    GtkWidget *lbl6 = gtk_label_new("Shortcuts & Categories Manager (~/.config/ntbox/shortcuts.json)");
    gtk_box_pack_start(GTK_BOX(tab6), lbl6, FALSE, FALSE, 0);
    GtkWidget *btn_short = gtk_button_new_with_label("Manage Shortcuts (Games, Steam, etc.)");
    gtk_box_pack_start(GTK_BOX(tab6), btn_short, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), tab6, gtk_label_new("Shortcuts"));
    
    gtk_widget_show_all(window);
    log_append("NT-Box Control Panel started");
    load_kernel_module_if_needed();
    gtk_main();
    
    return 0;
}
