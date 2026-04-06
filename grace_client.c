// gcc grace_client.c -o client `pkg-config --cflags --libs gtk+-3.0` -pthread

#define _POSIX_C_SOURCE 200809L
#include <gtk/gtk.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>

static GtkWidget *entry_host;
static GtkWidget *entry_port;
static GtkWidget *entry_name;
static GtkWidget *entry_recipient;
static GtkWidget *entry_msg;
static GtkWidget *entry_option;
static GtkWidget *text_view;
static GtkWidget *btn_connect;

static GtkWidget *entry_atk;
static GtkWidget *entry_def;
static GtkWidget *entry_reg;

static int sockfd = -1;
static pthread_t net_thread;
static volatile int running = 0;

// helper to update screen when other things change
struct ui_msg_data { char *msg; };

static gboolean ui_append_idle(gpointer data) {
    struct ui_msg_data *d = (struct ui_msg_data*)data;
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, d->msg, -1);
    gtk_text_buffer_insert(buf, &end, "\n", -1);
    free(d->msg);
    free(d);
    return G_SOURCE_REMOVE;
}

static void ui_append(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[2048];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    struct ui_msg_data *d = malloc(sizeof(*d));
    if (!d) return;
    d->msg = strdup(tmp);
    if (!d->msg) { free(d); return; }
    g_idle_add(ui_append_idle, d);
}

// Read/Write helpers
static int read_exact(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char*)buf;
    while (total < len) {
        ssize_t n = recv(fd, p + total, len - total, MSG_WAITALL);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = buf;
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int read_u16_le(int fd, uint16_t *out) {
    unsigned char b[2];
    if (read_exact(fd, b, 2) < 0) return -1;
    *out = (uint16_t)(b[0] | (b[1] << 8));
    return 0;
}

static int read_i16_le(int fd, int16_t *out) {
    unsigned char b[2];
    if (read_exact(fd, b, 2) < 0) return -1;
    *out = (int16_t)(b[0] | (b[1] << 8));
    return 0;
}

static void write_u16_le_buf(uint16_t v, unsigned char out[2]) {
    out[0] = (unsigned char)(v & 0xff);
    out[1] = (unsigned char)((v >> 8) & 0xff);
}

// Helper functions for reading in
static void handle_version() {
    uint8_t maj, min;
    uint16_t extlen;
    if (read_exact(sockfd, &maj, 1) < 0) { ui_append("Disconnected while reading VERSION"); return; }
    if (read_exact(sockfd, &min, 1) < 0) return;
    if (read_u16_le(sockfd, &extlen) < 0) return;

    ui_append("[VERSION] LURK %u.%u  extensions bytes=%u", maj, min, (unsigned)extlen);

    
    uint16_t remaining = extlen;
    while (remaining >= 2) {
        uint16_t elen;
        if (read_u16_le(sockfd, &elen) < 0) return;
        remaining -= 2;
        if (elen > remaining) elen = remaining;
        char *ext = malloc(elen + 1);
        if (!ext) return;
        if (elen > 0) {
            if (read_exact(sockfd, ext, elen) < 0) { free(ext); return; }
        }
        ext[elen] = '\0';
        remaining -= elen;
        ui_append("  extension: %s", ext);
        free(ext);
    }
}

static void handle_game() {
    uint16_t init_pts, stat_limit, desc_len;
    if (read_u16_le(sockfd, &init_pts) < 0) return;
    if (read_u16_le(sockfd, &stat_limit) < 0) return;
    if (read_u16_le(sockfd, &desc_len) < 0) return;
    char *desc = malloc(desc_len + 1);
    if (!desc) return;
    if (desc_len > 0) {
        if (read_exact(sockfd, desc, desc_len) < 0) { free(desc); return; }
    }
    desc[desc_len] = '\0';
    ui_append("[GAME] init=%u stat_limit=%u\n%s", (unsigned)init_pts, (unsigned)stat_limit, desc);
    free(desc);
}

static void handle_room_or_connection(const char *label) {
    uint16_t id, desc_len;
    char name[33] = {0};
    if (read_u16_le(sockfd, &id) < 0) return;
    if (read_exact(sockfd, name, 32) < 0) return;
    name[32] = '\0';
    if (read_u16_le(sockfd, &desc_len) < 0) return;
    char *desc = malloc(desc_len + 1);
    if (!desc) return;
    if (desc_len > 0) {
        if (read_exact(sockfd, desc, desc_len) < 0) { free(desc); return; }
    }
    desc[desc_len] = '\0';
    ui_append("[%s] id=%u name='%s' desc='%s'", label, (unsigned)id, name, desc);
    free(desc);
}

static void handle_character() {
    char name[33] = {0};
    uint8_t flags = 0;
    uint16_t atk=0, def=0, reg=0, gold=0, room=0;
    int16_t hp=0;
    uint16_t desc_len = 0;

    if (read_exact(sockfd, name, 32) < 0) return; name[32] = '\0';
    if (read_exact(sockfd, &flags, 1) < 0) return;
    if (read_u16_le(sockfd, &atk) < 0) return;
    if (read_u16_le(sockfd, &def) < 0) return;
    if (read_u16_le(sockfd, &reg) < 0) return;
    if (read_i16_le(sockfd, &hp) < 0) return;
    if (read_u16_le(sockfd, &gold) < 0) return;
    if (read_u16_le(sockfd, &room) < 0) return;
    if (read_u16_le(sockfd, &desc_len) < 0) return;

    char *desc = NULL;
    if (desc_len > 0) {
        desc = malloc(desc_len + 1);
        if (!desc) return;
        if (read_exact(sockfd, desc, desc_len) < 0) { free(desc); return; }
        desc[desc_len] = '\0';
    }

    int alive = (flags & 0x80) ? 1 : 0;
    int join_battle = (flags & 0x20) ? 1 : 0;
    int monster = (flags & 0x40) ? 1 : 0;
    int started = (flags & 0x10) ? 1 : 0;
    int ready = (flags & 0x08) ? 1 : 0;
    ui_append("[CHARACTER] -- name='%s' flags=0x%02x (alive=%d join=%d monster=%d started=%d ready=%d) atk=%u def=%u reg=%u hp=%d gold=%u room=%u desc='%s'",
              name, flags, alive, join_battle, monster, started, ready, (unsigned)atk, (unsigned)def, (unsigned)reg, (int)hp, (unsigned)gold, (unsigned)room, desc?desc:"");

    if (desc) free(desc);
}

static void handle_message() {
    uint16_t msg_len;
    char recipient[33] = {0}, sender[31] = {0};
    uint16_t narr;
    if (read_u16_le(sockfd, &msg_len) < 0) return;
    if (read_exact(sockfd, recipient, 32) < 0) return; recipient[32]='\0';
    if (read_exact(sockfd, sender, 30) < 0) return; sender[30]='\0';
    if (read_u16_le(sockfd, &narr) < 0) return;
    char *msg = malloc(msg_len + 1);
    if (!msg) return;
    if (msg_len > 0) {
        if (read_exact(sockfd, msg, msg_len) < 0) { free(msg); return; }
    }
    msg[msg_len] = '\0';
    ui_append("[MESSAGE] -- %s -> %s : %s", sender, recipient, msg);
    free(msg);
}

static void handle_accept_or_error(uint8_t type) {
    if (type == 8) {
        uint8_t action;
        if (read_exact(sockfd, &action, 1) < 0) return;
        ui_append("[ACCEPT] -- action=%u", action);
    } else if (type == 7) {
        uint8_t errcode;
        uint16_t msglen;
        if (read_exact(sockfd, &errcode, 1) < 0) return;
        if (read_u16_le(sockfd, &msglen) < 0) return;
        char *msg = malloc(msglen + 1);
        if (!msg) return;
        if (msglen > 0) {
            if (read_exact(sockfd, msg, msglen) < 0) { free(msg); return; }
        }
        msg[msglen] = '\0';
        ui_append("[ERROR] code=%u msg=%s", errcode, msg);
        free(msg);
    }
}

// netowrk thread
static void *network_thread_fn(void *arg) {
    (void)arg;
    ui_append("Network thread started (fd=%d)", sockfd);
    while (running) {
        uint8_t type;
        if (read_exact(sockfd, &type, 1) < 0) {
            ui_append("Connection closed: %s", strerror(errno));
            break;
        }
    // handing the type and what to send
        switch (type) {
            case 14: handle_version(); break;
            case 11: handle_game(); break;
            case 9:  handle_room_or_connection("ROOM"); break;
            case 13: handle_room_or_connection("CONNECTION"); break;
            case 10: handle_character(); break;
            case 1:  handle_message(); break;
            case 8:
            case 7:  handle_accept_or_error(type); break;
            default:
                ui_append("[UNKNOWN TYPE %u]", (unsigned)type);
                break;
        }
    }
    running = 0;
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
    ui_append("Network thread exiting.");
    return NULL;
}

// Outgoing packets
static void send_chat_packet(const char *recipient, const char *sender, const char *msg) {
    if (sockfd < 0) { ui_append("Not connected"); return; }
    uint8_t type = 1;
    uint16_t mlen = (uint16_t)strlen(msg);
    unsigned char lenbuf[2];
    write_u16_le_buf(mlen, lenbuf);

    char recbuf[32] = {0};
    char sdbuf[30] = {0};
    if (recipient) strncpy(recbuf, recipient, 31);
    if (sender) strncpy(sdbuf, sender, 29);

    if (write_all(sockfd, &type, 1) < 0) { ui_append("send error"); return; }
    if (write_all(sockfd, lenbuf, 2) < 0) { ui_append("send error"); return; }
    if (write_all(sockfd, recbuf, 32) < 0) { ui_append("send error"); return; }
    if (write_all(sockfd, sdbuf, 30) < 0) { ui_append("send error"); return; }
    unsigned char narrbuf[2]; write_u16_le_buf(0, narrbuf);
    if (write_all(sockfd, narrbuf, 2) < 0) { ui_append("send error"); return; }
    if (mlen && write_all(sockfd, msg, mlen) < 0) { ui_append("send error"); return; }

    ui_append("[SENT CHAT] -- %s -> %s : %s", sender?sender:"(me)", recipient?recipient:"(all)", msg);
}

static void send_character_packet(void) {
    if (sockfd < 0) { ui_append("Not connected"); return; }
    uint8_t type = 10;
    char namebuf[32] = {0};
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry_name));
    if (!name || name[0] == '\0') name = "unnamed";
    strncpy(namebuf, name, 31);

    uint8_t flags = 0;

    
    uint16_t atk = 0, def = 0, reg = 0;
    const char *atk_str = gtk_entry_get_text(GTK_ENTRY(entry_atk));
    const char *def_str = gtk_entry_get_text(GTK_ENTRY(entry_def));
    const char *reg_str = gtk_entry_get_text(GTK_ENTRY(entry_reg));
    
    atk = (uint16_t)strtoul(atk_str, NULL, 10);
    def = (uint16_t)strtoul(def_str, NULL, 10);
    reg = (uint16_t)strtoul(reg_str, NULL, 10);

    // Default to a base value if input is 0 or invalid
    if (atk == 0) atk = 10;
    if (def == 0) def = 10;
    
    int16_t hp = 100;
    uint16_t gold = 0;
    uint16_t room = 1;
    uint16_t desc_len = 0;
    unsigned char tmp[2];

    // dealing with issues in socket reading writing
    if (write_all(sockfd, &type, 1) < 0) { ui_append("send error"); return; }
    if (write_all(sockfd, namebuf, 32) < 0) { ui_append("send error"); return; }
    if (write_all(sockfd, &flags, 1) < 0) { ui_append("send error"); return; }
    write_u16_le_buf(atk, tmp); write_all(sockfd, tmp, 2);
    write_u16_le_buf(def, tmp); write_all(sockfd, tmp, 2);
    write_u16_le_buf(reg, tmp); write_all(sockfd, tmp, 2);
    write_u16_le_buf((uint16_t)hp, tmp); write_all(sockfd, tmp, 2);
    write_u16_le_buf(gold, tmp); write_all(sockfd, tmp, 2);
    write_u16_le_buf(room, tmp); write_all(sockfd, tmp, 2);
    write_u16_le_buf(desc_len, tmp); write_all(sockfd, tmp, 2);

    ui_append("[SENT CHARACTER] -- name=%s atk=%u def=%u reg=%u hp=%d room=%u",
              namebuf, (unsigned)atk, (unsigned)def, (unsigned)reg, (int)hp, (unsigned)room);
}

static void send_start(void) {
    if (sockfd < 0) { ui_append("Not connected"); return; }
    uint8_t type = 6;
    if (write_all(sockfd, &type, 1) < 0) { ui_append("send error"); return; }
    ui_append("[SENT START]");
}

static void send_changeroom(uint16_t target_room) {
    if (sockfd < 0) { ui_append("Not connected"); return; }
    uint8_t type = 2;
    unsigned char buf[2];
    write_u16_le_buf(target_room, buf);
    if (write_all(sockfd, &type, 1) < 0) { ui_append("send error"); return; }
    if (write_all(sockfd, buf, 2) < 0) { ui_append("send error"); return; }
    ui_append("[SENT CHANGEROOM] -- -> %u", (unsigned)target_room);
}

static void send_raw_hex(const char *hexline) {
    if (sockfd < 0) { ui_append("Not connected"); return; }
    char *copy = strdup(hexline);
    if (!copy) return;
    char *tok = strtok(copy, " \t");
    unsigned char out[1024];
    size_t idx = 0;
    while (tok && idx < sizeof(out)) {
        unsigned int v;
        if (sscanf(tok, "%x", &v) == 1) out[idx++] = (unsigned char)(v & 0xff);
        tok = strtok(NULL, " \t");
    }
    free(copy);
    if (idx == 0) { ui_append("No bytes parsed"); return; }
    if (write_all(sockfd, out, idx) < 0) { ui_append("send error"); return; }
    ui_append("[SENT RAW %zu bytes]", idx);
}

static void on_connect_clicked(GtkButton *b, gpointer user_data) {
    (void)b; (void)user_data;
    if (sockfd >= 0) {
        ui_append("Disconnecting...");
        running = 0;
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        sockfd = -1;
        gtk_button_set_label(GTK_BUTTON(btn_connect), "Connect");
        return;
    }

    const char *host = gtk_entry_get_text(GTK_ENTRY(entry_host));
    const char *portstr = gtk_entry_get_text(GTK_ENTRY(entry_port));
    int port = atoi(portstr);
    if (!host || host[0] == '\0') host = "74.118.22.194";
    //if (port <= 0) port = 5075;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { ui_append("socket() failed: %s", strerror(errno)); return; }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &serv.sin_addr) <= 0) {
        ui_append("Invalid host");
        close(s);
        return;
    }

    if (connect(s, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        ui_append("connect() failed: %s", strerror(errno));
        close(s);
        return;
    }

    sockfd = s;
    running = 1;
    if (pthread_create(&net_thread, NULL, network_thread_fn, NULL) != 0) {
        ui_append("pthread_create failed");
        running = 0;
        close(sockfd);
        sockfd = -1;
        return;
    }
    pthread_detach(net_thread);
    gtk_button_set_label(GTK_BUTTON(btn_connect), "Disconnect");
    ui_append("Connected to %s:%d (fd=%d)", host, port, sockfd);
}

static void on_send_clicked(GtkButton *b, gpointer user_data) {
    (void)b; (void)user_data;
    const char *txt = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry_name));
    const char *recipient = gtk_entry_get_text(GTK_ENTRY(entry_recipient));
    const char *target = (!recipient || recipient[0] == '\0') ? "*" : recipient;
        
    if (!txt || txt[0] == '\0') return;
    if (!name || name[0] == '\0') name = "client";

    send_chat_packet(target, name, txt); 
    gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
}

static void on_send_char_clicked(GtkButton *b, gpointer user_data) {
    (void)b; (void)user_data;
    send_character_packet();
}

static void on_send_option_clicked(GtkButton *b, gpointer user_data) {
    (void)b; (void)user_data;
    const char *opt = gtk_entry_get_text(GTK_ENTRY(entry_option));
    if (!opt || opt[0] == '\0') return;

    // trim leading spaces
    while (*opt == ' ' || *opt == '\t') opt++;

    if (strcmp(opt, "6") == 0) { // START
        send_start();
        gtk_entry_set_text(GTK_ENTRY(entry_option), "");
        return;
    }

    if (opt[0] == '2') {
        unsigned int roomid = 0;
        if (sscanf(opt + 1, "%u", &roomid) == 1) {
            send_changeroom((uint16_t)roomid);
            gtk_entry_set_text(GTK_ENTRY(entry_option), "");
            return;
        } else {
            ui_append("Usage for room change: 2 <roomid> (e.g. `2 4`)");
            return;
        }
    }

    if (strncmp(opt, "raw ", 4) == 0) {
        send_raw_hex(opt + 4);
        gtk_entry_set_text(GTK_ENTRY(entry_option), "");
        return;
    }

    unsigned int u;
    if (sscanf(opt, "%u", &u) == 1 && u <= 255) {
        uint8_t bval = (uint8_t)u;
        if (write_all(sockfd, &bval, 1) < 0) { ui_append("send error"); return; }
        ui_append("[SENT SINGLE BYTE] 0x%02x", bval);
        gtk_entry_set_text(GTK_ENTRY(entry_option), "");
        return;
    }

    ui_append("Unrecognized option");
}

static void on_entry_option_activate(GtkEntry *e, gpointer _) {
    (void)e;
    on_send_option_clicked(NULL, NULL);
}

static void on_window_destroy(GtkWidget *w, gpointer _) {
    (void)w;
    if (sockfd >= 0) {
        running = 0;
        shutdown(sockfd, SHUT_RDWR);
        close(sockfd);
        sockfd = -1;
    }
    gtk_main_quit();
}

// main function
int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Grace Client");
    gtk_window_set_default_size(GTK_WINDOW(win), 1024, 600);
    gtk_window_set_resizable(GTK_WINDOW(win), TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), NULL);

    GtkWidget *main_v = gtk_vbox_new(FALSE, 6);
    gtk_container_add(GTK_CONTAINER(win), main_v);

    GtkWidget *top_h1 = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(main_v), top_h1, FALSE, FALSE, 6);

    gtk_box_pack_start(GTK_BOX(top_h1), gtk_label_new("Host:"), FALSE, FALSE, 3);
    entry_host = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_host), "74.118.22.194");
    gtk_box_pack_start(GTK_BOX(top_h1), entry_host, FALSE, FALSE, 3);

    gtk_box_pack_start(GTK_BOX(top_h1), gtk_label_new("Port:"), FALSE, FALSE, 3);
    entry_port = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_port), "5075");
    gtk_widget_set_size_request(entry_port, 90, -1);
    gtk_box_pack_start(GTK_BOX(top_h1), entry_port, FALSE, FALSE, 3);

    btn_connect = gtk_button_new_with_label("Connect");
    gtk_box_pack_start(GTK_BOX(top_h1), btn_connect, FALSE, FALSE, 3);
    g_signal_connect(btn_connect, "clicked", G_CALLBACK(on_connect_clicked), NULL);

  
    //entry_name = gtk_entry_new();
    //gtk_entry_set_text(GTK_ENTRY(entry_name), "");
    //gtk_entry_set_placeholder_text(GTK_ENTRY(entry_name), "player name");
    //gtk_box_pack_start(GTK_BOX(top_h1), entry_name, TRUE, TRUE, 3);

    // make different row
    GtkWidget *top_h2 = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(main_v), top_h2, FALSE, FALSE, 6);

    entry_name = gtk_entry_new();
    //gtk_entry_set_text(GTK_ENTRY(entry_name), "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_name), "Player Name");
    gtk_box_pack_start(GTK_BOX(top_h1), entry_name, TRUE, TRUE, 3);


    // Send Character Button
    GtkWidget *btn_char = gtk_button_new_with_label("Send Character");
    gtk_box_pack_start(GTK_BOX(top_h2), btn_char, FALSE, FALSE, 3);
    g_signal_connect(btn_char, "clicked", G_CALLBACK(on_send_char_clicked), NULL);

    // ATTACK
    gtk_box_pack_start(GTK_BOX(top_h2), gtk_label_new("Attack:"), FALSE, FALSE, 3);
    entry_atk = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_atk), "0");
    gtk_widget_set_size_request(entry_atk, 40, -1);
    gtk_box_pack_start(GTK_BOX(top_h2), entry_atk, FALSE, FALSE, 3);

    // DEFENSE
    gtk_box_pack_start(GTK_BOX(top_h2), gtk_label_new("Defense:"), FALSE, FALSE, 3);
    entry_def = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_def), "0");
    gtk_widget_set_size_request(entry_def, 0, -1);
    gtk_box_pack_start(GTK_BOX(top_h2), entry_def, FALSE, FALSE, 3);

    // REGEN
    gtk_box_pack_start(GTK_BOX(top_h2), gtk_label_new("Regen:"), FALSE, FALSE, 3);
    entry_reg = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_reg), "0");
    gtk_widget_set_size_request(entry_reg, 0, -1);
    gtk_box_pack_start(GTK_BOX(top_h2), entry_reg, FALSE, FALSE, 3);
    
    // Add spacer to push controls to the left
    GtkWidget *spacer = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(top_h2), spacer, TRUE, TRUE, 3);

    // Log view
    text_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sc), text_view);
    gtk_box_pack_start(GTK_BOX(main_v), sc, TRUE, TRUE, 3);

    // Message entry and sending
    GtkWidget *bot_h = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(main_v), bot_h, FALSE, FALSE, 3);    
    gtk_box_pack_start(GTK_BOX(bot_h), gtk_label_new("To:"), FALSE, FALSE, 3);
    entry_recipient = gtk_entry_new();
    //gtk_entry_set_text(GTK_ENTRY(entry_recipient), "*"); // Default to all
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_recipient), "Recipient Name");
    gtk_widget_set_size_request(entry_recipient, 80, -1);
    gtk_box_pack_start(GTK_BOX(bot_h), entry_recipient, FALSE, FALSE, 3);
        
    entry_msg = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_msg), "Type message here: ");
    gtk_box_pack_start(GTK_BOX(bot_h), entry_msg, TRUE, TRUE, 3);
    GtkWidget *btn_send = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(bot_h), btn_send, FALSE, FALSE, 3);
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(entry_msg, "activate", G_CALLBACK(on_send_clicked), NULL);

    // Options row
    gtk_box_pack_start(GTK_BOX(main_v), gtk_label_new("Lurk message: "), FALSE, FALSE, 3);
    GtkWidget *opt_h = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(main_v), opt_h, FALSE, FALSE, 3);
    entry_option = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_option), "Type Lurk protocol here: ");
    gtk_box_pack_start(GTK_BOX(opt_h), entry_option, TRUE, TRUE, 3);
    GtkWidget *btn_option = gtk_button_new_with_label("Send");
    gtk_box_pack_start(GTK_BOX(opt_h), btn_option, FALSE, FALSE, 3);
    g_signal_connect(btn_option, "clicked", G_CALLBACK(on_send_option_clicked), NULL);
    g_signal_connect(entry_option, "activate", G_CALLBACK(on_entry_option_activate), NULL);

    gtk_widget_show_all(win);
    gtk_main();

    // clean up on exit
    running = 0;
    if (sockfd >= 0) { shutdown(sockfd, SHUT_RDWR); close(sockfd); }
    return 0;
}

// gcc grace_client.c -o client `pkg-config --cflags --libs gtk+-3.0` -pthread
