#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#define TARGET_VENDOR_ID  0x0810
#define TARGET_PRODUCT_ID 0xe501
#define VIRTUAL_NAME      "SNES Virtual Controller & Keyboard"

#define AXIS_MIN_THRESHOLD 64
#define AXIS_MAX_THRESHOLD 192

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

typedef enum {
    MODE_KEYBOARD = 0,  /* FBNeo / Arcade keyboard simulation */
    MODE_GAMEPAD,       /* Virtual gamepad buttons & hat */
    MODE_HYBRID         /* Both keyboard & gamepad simultaneously */
} emit_mode_t;

/* 12 button indices */
enum {
    BTN_IDX_UP = 0,
    BTN_IDX_DOWN,
    BTN_IDX_LEFT,
    BTN_IDX_RIGHT,
    BTN_IDX_B,
    BTN_IDX_A,
    BTN_IDX_Y,
    BTN_IDX_X,
    BTN_IDX_L,
    BTN_IDX_R,
    BTN_IDX_SELECT,
    BTN_IDX_START,
    BTN_IDX_COUNT
};

/* Button names */
static const char *g_btn_names[BTN_IDX_COUNT] = {
    "Croix HAUT", "Croix BAS", "Croix GAUCHE", "Croix DROITE",
    "Bouton B", "Bouton A", "Bouton Y", "Bouton X",
    "Gâchette L", "Gâchette R", "Bouton SELECT", "Bouton START"
};

/* Default keyboard key mapping: Fightcade / Street Fighter III (with A instead of Control) */
static int g_custom_keys[BTN_IDX_COUNT] = {
    KEY_UP,        /* UP    -> Arrow Up */
    KEY_DOWN,      /* DOWN  -> Arrow Down */
    KEY_LEFT,      /* LEFT  -> Arrow Left */
    KEY_RIGHT,     /* RIGHT -> Arrow Right */
    KEY_Z,         /* B     -> LK (Weak Kick) */
    KEY_X,         /* A     -> MK (Medium Kick) */
    KEY_A,         /* Y     -> LP (Weak Punch - requested key A) */
    KEY_S,         /* X     -> MP (Medium Punch) */
    KEY_D,         /* L     -> HP (Strong Punch) */
    KEY_C,         /* R     -> HK (Strong Kick) */
    KEY_5,         /* SELECT-> Coin / Crédit */
    KEY_1          /* START -> 1P Start */
};

typedef struct {
    int dev_fd;
    int uinput_fd;
    char dev_path[256];
    char dev_name[256];
    uint16_t vendor;
    uint16_t product;
    
    bool up;
    bool down;
    bool left;
    bool right;
    
    bool uinput_enabled;
    emit_mode_t mode;
    pthread_mutex_t lock;
} snes_backend_t;

typedef struct {
    GtkWidget *window;
    GtkWidget *status_badge;
    GtkWidget *device_info_label;
    GtkWidget *uinput_switch;
    GtkWidget *mode_combo;
    GtkWidget *event_log_label;
    GtkWidget *buttons[BTN_IDX_COUNT];
} snes_gui_t;

static snes_backend_t g_backend;
static snes_gui_t g_gui;
static pthread_t g_worker_thread;
static bool g_gui_active = false;

/* Config File Persistence */
static void get_config_path(char *out, size_t size) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, size, "%s/.config/snescontrol/mapping.conf", home);
}

static void save_key_mapping(void) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/.config/snescontrol", getenv("HOME") ? getenv("HOME") : "/tmp");
    mkdir(dir, 0755);

    char path[512];
    get_config_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < BTN_IDX_COUNT; i++) {
        fprintf(f, "%d=%d\n", i, g_custom_keys[i]);
    }
    fclose(f);
    printf("[*] Configuration des touches sauvegardée dans %s\n", path);
}

static void load_key_mapping(void) {
    char path[512];
    get_config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int idx = -1, key = -1;
        if (sscanf(line, "%d=%d", &idx, &key) == 2) {
            if (idx >= 0 && idx < BTN_IDX_COUNT && key > 0 && key < 255) {
                g_custom_keys[idx] = key;
            }
        }
    }
    fclose(f);
    printf("[*] Configuration des touches chargée depuis %s\n", path);
}

static const char *get_key_name(int key) {
    switch (key) {
        case KEY_A: return "A";
        case KEY_B: return "B";
        case KEY_C: return "C";
        case KEY_D: return "D";
        case KEY_E: return "E";
        case KEY_F: return "F";
        case KEY_G: return "G";
        case KEY_H: return "H";
        case KEY_I: return "I";
        case KEY_J: return "J";
        case KEY_K: return "K";
        case KEY_L: return "L";
        case KEY_M: return "M";
        case KEY_N: return "N";
        case KEY_O: return "O";
        case KEY_P: return "P";
        case KEY_Q: return "Q";
        case KEY_R: return "R";
        case KEY_S: return "S";
        case KEY_T: return "T";
        case KEY_U: return "U";
        case KEY_V: return "V";
        case KEY_W: return "W";
        case KEY_X: return "X";
        case KEY_Y: return "Y";
        case KEY_Z: return "Z";
        case KEY_0: return "0";
        case KEY_1: return "1";
        case KEY_2: return "2";
        case KEY_3: return "3";
        case KEY_4: return "4";
        case KEY_5: return "5";
        case KEY_6: return "6";
        case KEY_7: return "7";
        case KEY_8: return "8";
        case KEY_9: return "9";
        case KEY_UP: return "Haut";
        case KEY_DOWN: return "Bas";
        case KEY_LEFT: return "Gauche";
        case KEY_RIGHT: return "Droite";
        case KEY_LEFTCTRL: return "Ctrl-G";
        case KEY_RIGHTCTRL: return "Ctrl-D";
        case KEY_LEFTALT: return "Alt-G";
        case KEY_RIGHTALT: return "Alt-D";
        case KEY_LEFTSHIFT: return "Shift-G";
        case KEY_RIGHTSHIFT: return "Shift-D";
        case KEY_SPACE: return "Espace";
        case KEY_ENTER: return "Entrée";
        case KEY_TAB: return "Tab";
        case KEY_BACKSPACE: return "Effacer";
        case KEY_ESC: return "Echap";
        default: {
            static char buf[32];
            snprintf(buf, sizeof(buf), "Key %d", key);
            return buf;
        }
    }
}

/* Map physical gamepad codes to virtual button index */
static int map_physical_to_idx(int physical_code) {
    switch (physical_code) {
        case 289: /* BTN_THUMB  */ return BTN_IDX_A;
        case 290: /* BTN_THUMB2 */ return BTN_IDX_B;
        case 288: /* BTN_TRIGGER*/ return BTN_IDX_X;
        case 291: /* BTN_TOP    */ return BTN_IDX_Y;
        case 292: /* BTN_TOP2   */ return BTN_IDX_L;
        case 293: /* BTN_PINKIE */ return BTN_IDX_R;
        case 294: /* BTN_BASE   */ return BTN_IDX_R;
        case 296: /* BTN_BASE3  */ return BTN_IDX_SELECT;
        case 297: /* BTN_BASE4  */ return BTN_IDX_START;
        default:  return -1;
    }
}

static int map_idx_to_virtual_btn(int idx) {
    switch (idx) {
        case BTN_IDX_A:      return BTN_A;
        case BTN_IDX_B:      return BTN_B;
        case BTN_IDX_X:      return BTN_X;
        case BTN_IDX_Y:      return BTN_Y;
        case BTN_IDX_L:      return BTN_TL;
        case BTN_IDX_R:      return BTN_TR;
        case BTN_IDX_SELECT: return BTN_SELECT;
        case BTN_IDX_START:  return BTN_START;
        case BTN_IDX_UP:     return BTN_DPAD_UP;
        case BTN_IDX_DOWN:   return BTN_DPAD_DOWN;
        case BTN_IDX_LEFT:   return BTN_DPAD_LEFT;
        case BTN_IDX_RIGHT:  return BTN_DPAD_RIGHT;
        default:             return -1;
    }
}

static void emit_event(int fd, uint16_t type, uint16_t code, int32_t value) {
    if (fd < 0) return;
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        /* Ignore transient write errors */
    }
}

static void sync_events(int fd) {
    emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static bool is_target_device(const char *path, uint16_t *out_vendor, uint16_t *out_product, char *out_name, size_t name_size) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;

    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) < 0) {
        close(fd);
        return false;
    }

    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        strncpy(name, "Unknown", sizeof(name) - 1);
    }
    close(fd);

    if (strstr(name, "Virtual") != NULL || id.product == 0xe502) {
        return false;
    }

    bool match = false;
    if (id.vendor == TARGET_VENDOR_ID && id.product == TARGET_PRODUCT_ID) {
        match = true;
    } else if (strstr(name, "usb gamepad") != NULL || strstr(name, "SNES Gamepad") != NULL) {
        match = true;
    }

    if (match) {
        if (out_vendor) *out_vendor = id.vendor;
        if (out_product) *out_product = id.product;
        if (out_name) strncpy(out_name, name, name_size - 1);
        return true;
    }

    return false;
}

static bool find_gamepad(char *found_path, size_t path_size, uint16_t *vendor, uint16_t *product, char *name, size_t name_size) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return false;

    struct dirent *ent;
    bool found = false;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) == 0) {
            if ((size_t)snprintf(found_path, path_size, "/dev/input/%s", ent->d_name) < path_size) {
                if (is_target_device(found_path, vendor, product, name, name_size)) {
                    found = true;
                    break;
                }
            }
        }
    }

    closedir(dir);
    return found;
}

static int create_uinput_device(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("[-] Failed to open /dev/uinput");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);

    /* Gamepad action buttons */
    ioctl(fd, UI_SET_KEYBIT, BTN_A);
    ioctl(fd, UI_SET_KEYBIT, BTN_B);
    ioctl(fd, UI_SET_KEYBIT, BTN_X);
    ioctl(fd, UI_SET_KEYBIT, BTN_Y);
    ioctl(fd, UI_SET_KEYBIT, BTN_TL);
    ioctl(fd, UI_SET_KEYBIT, BTN_TR);
    ioctl(fd, UI_SET_KEYBIT, BTN_SELECT);
    ioctl(fd, UI_SET_KEYBIT, BTN_START);

    /* Digital D-Pad buttons */
    ioctl(fd, UI_SET_KEYBIT, BTN_DPAD_UP);
    ioctl(fd, UI_SET_KEYBIT, BTN_DPAD_DOWN);
    ioctl(fd, UI_SET_KEYBIT, BTN_DPAD_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_DPAD_RIGHT);

    /* Gamepad Hat axis */
    ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
    ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);

    struct uinput_abs_setup abs_setup;
    memset(&abs_setup, 0, sizeof(abs_setup));
    abs_setup.code = ABS_HAT0X;
    abs_setup.absinfo.minimum = -1;
    abs_setup.absinfo.maximum = 1;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);

    abs_setup.code = ABS_HAT0Y;
    abs_setup.absinfo.minimum = -1;
    abs_setup.absinfo.maximum = 1;
    abs_setup.absinfo.value = 0;
    ioctl(fd, UI_ABS_SETUP, &abs_setup);

    /* Enable ALL keyboard keys so user can rebind to ANY key */
    for (int k = 1; k < 255; k++) {
        ioctl(fd, UI_SET_KEYBIT, k);
    }

    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = TARGET_VENDOR_ID;
    usetup.id.product = 0xe502;
    usetup.id.version = 1;
    strncpy(usetup.name, VIRTUAL_NAME, UINPUT_MAX_NAME_SIZE);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("[-] Failed to create uinput device");
        close(fd);
        return -1;
    }

    printf("[+] Created virtual gamepad & keyboard: %s\n", VIRTUAL_NAME);
    return fd;
}

static void destroy_uinput_device(int *fd) {
    if (fd && *fd >= 0) {
        ioctl(*fd, UI_DEV_DESTROY);
        close(*fd);
        *fd = -1;
        printf("[-] Destroyed virtual device\n");
    }
}

/* UI update data dispatched to GTK main loop */
typedef struct {
    int btn_idx;
    bool pressed;
    char log_text[128];
    bool update_status;
    bool connected;
    char dev_info[600];
} ui_update_data_t;

static gboolean on_ui_update(gpointer user_data) {
    ui_update_data_t *data = (ui_update_data_t *)user_data;
    if (!g_gui_active) {
        free(data);
        return G_SOURCE_REMOVE;
    }

    if (data->update_status) {
        if (data->connected) {
            gtk_label_set_text(GTK_LABEL(g_gui.status_badge), "CONNECTÉ");
            GtkStyleContext *ctx = gtk_widget_get_style_context(g_gui.status_badge);
            gtk_style_context_remove_class(ctx, "badge-disconnected");
            gtk_style_context_add_class(ctx, "badge-connected");
            gtk_label_set_text(GTK_LABEL(g_gui.device_info_label), data->dev_info);
        } else {
            gtk_label_set_text(GTK_LABEL(g_gui.status_badge), "DÉCONNECTÉ");
            GtkStyleContext *ctx = gtk_widget_get_style_context(g_gui.status_badge);
            gtk_style_context_remove_class(ctx, "badge-connected");
            gtk_style_context_add_class(ctx, "badge-disconnected");
            gtk_label_set_text(GTK_LABEL(g_gui.device_info_label), "Branchez une manette SNES USB pour commencer.");
        }
    }

    if (data->btn_idx >= 0 && data->btn_idx < BTN_IDX_COUNT && g_gui.buttons[data->btn_idx]) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(g_gui.buttons[data->btn_idx]);
        if (data->pressed) {
            gtk_style_context_add_class(ctx, "active-highlight");
        } else {
            gtk_style_context_remove_class(ctx, "active-highlight");
        }
    }

    if (data->log_text[0] != '\0') {
        gtk_label_set_text(GTK_LABEL(g_gui.event_log_label), data->log_text);
    }

    free(data);
    return G_SOURCE_REMOVE;
}

static void dispatch_ui_btn(int btn_idx, bool pressed, const char *log_text) {
    if (!g_gui_active) return;
    ui_update_data_t *d = calloc(1, sizeof(ui_update_data_t));
    if (!d) return;
    d->btn_idx = btn_idx;
    d->pressed = pressed;
    if (log_text) snprintf(d->log_text, sizeof(d->log_text), "%s", log_text);
    g_idle_add(on_ui_update, d);
}

static void dispatch_ui_status(bool connected, const char *dev_info) {
    if (!g_gui_active) return;
    ui_update_data_t *d = calloc(1, sizeof(ui_update_data_t));
    if (!d) return;
    d->btn_idx = -1;
    d->update_status = true;
    d->connected = connected;
    if (dev_info) snprintf(d->dev_info, sizeof(d->dev_info), "%s", dev_info);
    g_idle_add(on_ui_update, d);
}

static bool attach_gamepad(snes_backend_t *backend, const char *path) {
    pthread_mutex_lock(&backend->lock);
    strncpy(backend->dev_path, path, sizeof(backend->dev_path) - 1);
    backend->dev_fd = open(backend->dev_path, O_RDONLY | O_NONBLOCK);
    if (backend->dev_fd < 0) {
        pthread_mutex_unlock(&backend->lock);
        return false;
    }

    if (ioctl(backend->dev_fd, EVIOCGNAME(sizeof(backend->dev_name) - 1), backend->dev_name) < 0) {
        strncpy(backend->dev_name, "SNES Gamepad", sizeof(backend->dev_name) - 1);
    }

    struct input_id id;
    if (ioctl(backend->dev_fd, EVIOCGID, &id) == 0) {
        backend->vendor = id.vendor;
        backend->product = id.product;
    }

    backend->up = false;
    backend->down = false;
    backend->left = false;
    backend->right = false;

    if (backend->uinput_enabled) {
        ioctl(backend->dev_fd, EVIOCGRAB, 1);
        backend->uinput_fd = create_uinput_device();
    }

    char info[600];
    snprintf(info, sizeof(info), "%s (%04x:%04x) sur %s",
             backend->dev_name, backend->vendor, backend->product, backend->dev_path);
    printf("[+] Gamepad attached: %s\n", info);
    pthread_mutex_unlock(&backend->lock);

    dispatch_ui_status(true, info);
    return true;
}

static void detach_gamepad(snes_backend_t *backend) {
    pthread_mutex_lock(&backend->lock);
    if (backend->dev_fd >= 0) {
        ioctl(backend->dev_fd, EVIOCGRAB, 0);
        close(backend->dev_fd);
        backend->dev_fd = -1;
    }
    destroy_uinput_device(&backend->uinput_fd);
    pthread_mutex_unlock(&backend->lock);

    dispatch_ui_status(false, NULL);
    printf("[-] Gamepad detached\n");
}

static void set_uinput_enabled(snes_backend_t *backend, bool enabled) {
    pthread_mutex_lock(&backend->lock);
    backend->uinput_enabled = enabled;
    if (backend->dev_fd >= 0) {
        if (enabled && backend->uinput_fd < 0) {
            ioctl(backend->dev_fd, EVIOCGRAB, 1);
            backend->uinput_fd = create_uinput_device();
            printf("[+] Uinput enabled and physical device grabbed.\n");
        } else if (!enabled && backend->uinput_fd >= 0) {
            ioctl(backend->dev_fd, EVIOCGRAB, 0);
            destroy_uinput_device(&backend->uinput_fd);
            printf("[-] Uinput disabled and physical device ungrabbed.\n");
        }
    }
    pthread_mutex_unlock(&backend->lock);
}

static void set_emit_mode(snes_backend_t *backend, emit_mode_t mode) {
    pthread_mutex_lock(&backend->lock);
    backend->mode = mode;
    const char *mname = (mode == MODE_KEYBOARD) ? "Clavier (FBNeo)" :
                        (mode == MODE_GAMEPAD)  ? "Manette Virtuelle" : "Hybride (Les deux)";
    printf("[*] Mode d'émission configuré sur : %s\n", mname);
    pthread_mutex_unlock(&backend->lock);
}

static void process_events(snes_backend_t *backend) {
    struct input_event ev;
    ssize_t n = read(backend->dev_fd, &ev, sizeof(ev));
    if (n < (ssize_t)sizeof(ev)) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
        detach_gamepad(backend);
        return;
    }

    if (ev.type == EV_KEY) {
        int idx = map_physical_to_idx(ev.code);
        if (idx >= 0) {
            int vcode = map_idx_to_virtual_btn(idx);
            int kcode = g_custom_keys[idx];
            const char *bname = g_btn_names[idx];
            const char *kname = get_key_name(kcode);

            char log[128];
            snprintf(log, sizeof(log), "%s -> Touche Clavier '%s' : %s",
                     bname, kname, ev.value ? "PRESSÉ" : "RELÂCHÉ");
            dispatch_ui_btn(idx, ev.value != 0, log);

            pthread_mutex_lock(&backend->lock);
            if (backend->uinput_fd >= 0) {
                if (backend->mode == MODE_KEYBOARD || backend->mode == MODE_HYBRID) {
                    if (kcode > 0) emit_event(backend->uinput_fd, EV_KEY, kcode, ev.value);
                }
                if (backend->mode == MODE_GAMEPAD || backend->mode == MODE_HYBRID) {
                    if (vcode > 0) emit_event(backend->uinput_fd, EV_KEY, vcode, ev.value);
                }
                sync_events(backend->uinput_fd);
            }
            pthread_mutex_unlock(&backend->lock);
        }
    } else if (ev.type == EV_ABS) {
        bool changed = false;

        if (ev.code == ABS_X) {
            bool new_left = (ev.value < AXIS_MIN_THRESHOLD);
            bool new_right = (ev.value > AXIS_MAX_THRESHOLD);

            if (new_left != backend->left) {
                backend->left = new_left;
                char log[128];
                snprintf(log, sizeof(log), "Croix GAUCHE -> Touche '%s' : %s",
                         get_key_name(g_custom_keys[BTN_IDX_LEFT]), backend->left ? "PRESSÉ" : "RELÂCHÉ");
                dispatch_ui_btn(BTN_IDX_LEFT, backend->left, log);
                changed = true;
            }
            if (new_right != backend->right) {
                backend->right = new_right;
                char log[128];
                snprintf(log, sizeof(log), "Croix DROITE -> Touche '%s' : %s",
                         get_key_name(g_custom_keys[BTN_IDX_RIGHT]), backend->right ? "PRESSÉ" : "RELÂCHÉ");
                dispatch_ui_btn(BTN_IDX_RIGHT, backend->right, log);
                changed = true;
            }

            pthread_mutex_lock(&backend->lock);
            if (changed && backend->uinput_fd >= 0) {
                if (backend->mode == MODE_KEYBOARD || backend->mode == MODE_HYBRID) {
                    emit_event(backend->uinput_fd, EV_KEY, g_custom_keys[BTN_IDX_LEFT], backend->left ? 1 : 0);
                    emit_event(backend->uinput_fd, EV_KEY, g_custom_keys[BTN_IDX_RIGHT], backend->right ? 1 : 0);
                }
                if (backend->mode == MODE_GAMEPAD || backend->mode == MODE_HYBRID) {
                    emit_event(backend->uinput_fd, EV_KEY, BTN_DPAD_LEFT, backend->left ? 1 : 0);
                    emit_event(backend->uinput_fd, EV_KEY, BTN_DPAD_RIGHT, backend->right ? 1 : 0);
                    int hat_x = backend->right ? 1 : (backend->left ? -1 : 0);
                    emit_event(backend->uinput_fd, EV_ABS, ABS_HAT0X, hat_x);
                }
            }
            pthread_mutex_unlock(&backend->lock);
        } else if (ev.code == ABS_Y) {
            bool new_up = (ev.value < AXIS_MIN_THRESHOLD);
            bool new_down = (ev.value > AXIS_MAX_THRESHOLD);

            if (new_up != backend->up) {
                backend->up = new_up;
                char log[128];
                snprintf(log, sizeof(log), "Croix HAUT -> Touche '%s' : %s",
                         get_key_name(g_custom_keys[BTN_IDX_UP]), backend->up ? "PRESSÉ" : "RELÂCHÉ");
                dispatch_ui_btn(BTN_IDX_UP, backend->up, log);
                changed = true;
            }
            if (new_down != backend->down) {
                backend->down = new_down;
                char log[128];
                snprintf(log, sizeof(log), "Croix BAS -> Touche '%s' : %s",
                         get_key_name(g_custom_keys[BTN_IDX_DOWN]), backend->down ? "PRESSÉ" : "RELÂCHÉ");
                dispatch_ui_btn(BTN_IDX_DOWN, backend->down, log);
                changed = true;
            }

            pthread_mutex_lock(&backend->lock);
            if (changed && backend->uinput_fd >= 0) {
                if (backend->mode == MODE_KEYBOARD || backend->mode == MODE_HYBRID) {
                    emit_event(backend->uinput_fd, EV_KEY, g_custom_keys[BTN_IDX_UP], backend->up ? 1 : 0);
                    emit_event(backend->uinput_fd, EV_KEY, g_custom_keys[BTN_IDX_DOWN], backend->down ? 1 : 0);
                }
                if (backend->mode == MODE_GAMEPAD || backend->mode == MODE_HYBRID) {
                    emit_event(backend->uinput_fd, EV_KEY, BTN_DPAD_UP, backend->up ? 1 : 0);
                    emit_event(backend->uinput_fd, EV_KEY, BTN_DPAD_DOWN, backend->down ? 1 : 0);
                    int hat_y = backend->down ? 1 : (backend->up ? -1 : 0);
                    emit_event(backend->uinput_fd, EV_ABS, ABS_HAT0Y, hat_y);
                }
            }
            pthread_mutex_unlock(&backend->lock);
        }

        if (changed) {
            pthread_mutex_lock(&backend->lock);
            if (backend->uinput_fd >= 0) {
                sync_events(backend->uinput_fd);
            }
            pthread_mutex_unlock(&backend->lock);
        }
    }
}

/* Background worker thread for polling controller & inotify hotplug */
static void *worker_thread_func(void *arg) {
    (void)arg;
    int inotify_fd = inotify_init1(IN_NONBLOCK);
    int inotify_wd = -1;
    if (inotify_fd >= 0) {
        inotify_wd = inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB);
    }

    char found_path[256];
    if (find_gamepad(found_path, sizeof(found_path), NULL, NULL, NULL, 0)) {
        attach_gamepad(&g_backend, found_path);
    } else {
        dispatch_ui_status(false, NULL);
    }

    while (g_running) {
        struct pollfd fds[2];
        int nfds = 0;

        pthread_mutex_lock(&g_backend.lock);
        int cur_dev_fd = g_backend.dev_fd;
        pthread_mutex_unlock(&g_backend.lock);

        if (cur_dev_fd >= 0) {
            fds[nfds].fd = cur_dev_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (inotify_fd >= 0) {
            fds[nfds].fd = inotify_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        int poll_timeout = (cur_dev_fd < 0) ? 500 : -1;
        int ret = poll(fds, nfds, poll_timeout);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (cur_dev_fd >= 0) {
            for (int i = 0; i < nfds; i++) {
                if (fds[i].fd == cur_dev_fd && (fds[i].revents & (POLLIN | POLLERR | POLLHUP))) {
                    process_events(&g_backend);
                }
            }
        }

        bool check_hotplug = false;
        if (inotify_fd >= 0) {
            for (int i = 0; i < nfds; i++) {
                if (fds[i].fd == inotify_fd && (fds[i].revents & POLLIN)) {
                    char buf[1024];
                    while (read(inotify_fd, buf, sizeof(buf)) > 0);
                    check_hotplug = true;
                }
            }
        }

        pthread_mutex_lock(&g_backend.lock);
        bool needs_attach = (g_backend.dev_fd < 0);
        pthread_mutex_unlock(&g_backend.lock);

        if ((check_hotplug || poll_timeout > 0) && needs_attach) {
            if (find_gamepad(found_path, sizeof(found_path), NULL, NULL, NULL, 0)) {
                attach_gamepad(&g_backend, found_path);
            }
        }
    }

    detach_gamepad(&g_backend);
    if (inotify_wd >= 0) inotify_rm_watch(inotify_fd, inotify_wd);
    if (inotify_fd >= 0) close(inotify_fd);
    return NULL;
}

/* Update all button labels on the virtual controller */
static void update_all_button_labels(void) {
    if (!g_gui_active) return;
    for (int i = 0; i < BTN_IDX_COUNT; i++) {
        if (!g_gui.buttons[i]) continue;
        char text[64];
        const char *kname = get_key_name(g_custom_keys[i]);
        switch (i) {
            case BTN_IDX_UP:    snprintf(text, sizeof(text), "▲\n[%s]", kname); break;
            case BTN_IDX_DOWN:  snprintf(text, sizeof(text), "▼\n[%s]", kname); break;
            case BTN_IDX_LEFT:  snprintf(text, sizeof(text), "◄\n[%s]", kname); break;
            case BTN_IDX_RIGHT: snprintf(text, sizeof(text), "►\n[%s]", kname); break;
            case BTN_IDX_B:     snprintf(text, sizeof(text), "B\n[%s]", kname); break;
            case BTN_IDX_A:     snprintf(text, sizeof(text), "A\n[%s]", kname); break;
            case BTN_IDX_Y:     snprintf(text, sizeof(text), "Y\n[%s]", kname); break;
            case BTN_IDX_X:     snprintf(text, sizeof(text), "X\n[%s]", kname); break;
            case BTN_IDX_L:     snprintf(text, sizeof(text), "GÂCHETTE L\n[%s]", kname); break;
            case BTN_IDX_R:     snprintf(text, sizeof(text), "GÂCHETTE R\n[%s]", kname); break;
            case BTN_IDX_SELECT:snprintf(text, sizeof(text), "SELECT\n[%s]", kname); break;
            case BTN_IDX_START: snprintf(text, sizeof(text), "START\n[%s]", kname); break;
        }
        gtk_button_set_label(GTK_BUTTON(g_gui.buttons[i]), text);
    }
}

/* Key Capture Modal Dialog */
typedef struct {
    GtkWidget *dialog;
    int target_btn_idx;
    bool captured;
} key_capture_data_t;

static gboolean on_capture_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    key_capture_data_t *cap = (key_capture_data_t *)user_data;
    (void)widget;

    /* Convert X11 hardware keycode to Linux input keycode (offset is 8) */
    int linux_code = event->hardware_keycode - 8;
    if (linux_code > 0 && linux_code < 255) {
        g_custom_keys[cap->target_btn_idx] = linux_code;
        save_key_mapping();
        update_all_button_labels();
        cap->captured = true;
        gtk_dialog_response(GTK_DIALOG(cap->dialog), GTK_RESPONSE_ACCEPT);
        return TRUE;
    }
    return FALSE;
}

static void prompt_key_rebind(GtkWindow *parent, int btn_idx) {
    if (btn_idx < 0 || btn_idx >= BTN_IDX_COUNT) return;

    key_capture_data_t cap;
    cap.target_btn_idx = btn_idx;
    cap.captured = false;

    char title[128];
    snprintf(title, sizeof(title), "Assigner une touche : %s", g_btn_names[btn_idx]);

    cap.dialog = gtk_dialog_new_with_buttons(
        title,
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Annuler",
        GTK_RESPONSE_CANCEL,
        NULL);

    gtk_window_set_default_size(GTK_WINDOW(cap.dialog), 420, 160);
    gtk_window_set_position(GTK_WINDOW(cap.dialog), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(cap.dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 20);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Touche actuelle : <b>%s</b>\n\n<b>Appuyez sur la touche du clavier</b> que vous souhaitez assigner...",
             get_key_name(g_custom_keys[btn_idx]));

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), msg);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(content), label, TRUE, TRUE, 10);

    g_signal_connect(cap.dialog, "key-press-event", G_CALLBACK(on_capture_key_press), &cap);

    gtk_widget_show_all(cap.dialog);
    gtk_dialog_run(GTK_DIALOG(cap.dialog));
    gtk_widget_destroy(cap.dialog);
}

static void on_pad_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int idx = GPOINTER_TO_INT(user_data);
    prompt_key_rebind(GTK_WINDOW(g_gui.window), idx);
}

/* Full Key Configuration Dialog */
static void on_cfg_row_rebind(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int idx = GPOINTER_TO_INT(user_data);
    prompt_key_rebind(GTK_WINDOW(g_gui.window), idx);
}

static void apply_preset_fightcade(void) {
    /* Capcom 6-button SFIII on SNES controller with A instead of Control */
    g_custom_keys[BTN_IDX_UP]     = KEY_UP;
    g_custom_keys[BTN_IDX_DOWN]   = KEY_DOWN;
    g_custom_keys[BTN_IDX_LEFT]   = KEY_LEFT;
    g_custom_keys[BTN_IDX_RIGHT]  = KEY_RIGHT;
    g_custom_keys[BTN_IDX_Y]      = KEY_A; /* Weak Punch (LP) */
    g_custom_keys[BTN_IDX_X]      = KEY_S; /* Medium Punch (MP) */
    g_custom_keys[BTN_IDX_L]      = KEY_D; /* Strong Punch (HP) */
    g_custom_keys[BTN_IDX_B]      = KEY_Z; /* Weak Kick (LK) */
    g_custom_keys[BTN_IDX_A]      = KEY_X; /* Medium Kick (MK) */
    g_custom_keys[BTN_IDX_R]      = KEY_C; /* Strong Kick (HK) */
    g_custom_keys[BTN_IDX_SELECT] = KEY_5; /* Coin */
    g_custom_keys[BTN_IDX_START]  = KEY_1; /* Start */
    save_key_mapping();
    update_all_button_labels();
}

static void apply_preset_mame(void) {
    g_custom_keys[BTN_IDX_UP]     = KEY_UP;
    g_custom_keys[BTN_IDX_DOWN]   = KEY_DOWN;
    g_custom_keys[BTN_IDX_LEFT]   = KEY_LEFT;
    g_custom_keys[BTN_IDX_RIGHT]  = KEY_RIGHT;
    g_custom_keys[BTN_IDX_B]      = KEY_LEFTCTRL;
    g_custom_keys[BTN_IDX_A]      = KEY_LEFTALT;
    g_custom_keys[BTN_IDX_Y]      = KEY_SPACE;
    g_custom_keys[BTN_IDX_X]      = KEY_LEFTSHIFT;
    g_custom_keys[BTN_IDX_L]      = KEY_Z;
    g_custom_keys[BTN_IDX_R]      = KEY_X;
    g_custom_keys[BTN_IDX_SELECT] = KEY_5;
    g_custom_keys[BTN_IDX_START]  = KEY_1;
    save_key_mapping();
    update_all_button_labels();
}

static void apply_preset_letters(void) {
    g_custom_keys[BTN_IDX_UP]     = KEY_UP;
    g_custom_keys[BTN_IDX_DOWN]   = KEY_DOWN;
    g_custom_keys[BTN_IDX_LEFT]   = KEY_LEFT;
    g_custom_keys[BTN_IDX_RIGHT]  = KEY_RIGHT;
    g_custom_keys[BTN_IDX_A]      = KEY_A;
    g_custom_keys[BTN_IDX_B]      = KEY_B;
    g_custom_keys[BTN_IDX_X]      = KEY_X;
    g_custom_keys[BTN_IDX_Y]      = KEY_Y;
    g_custom_keys[BTN_IDX_L]      = KEY_L;
    g_custom_keys[BTN_IDX_R]      = KEY_R;
    g_custom_keys[BTN_IDX_SELECT] = KEY_SPACE;
    g_custom_keys[BTN_IDX_START]  = KEY_ENTER;
    save_key_mapping();
    update_all_button_labels();
}

static void open_config_dialog(GtkWindow *parent) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Configuration des Touches Clavier",
        parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Fermer",
        GTK_RESPONSE_CLOSE,
        NULL);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 580, 500);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);

    /* Presets bar */
    GtkWidget *preset_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *preset_lbl = gtk_label_new("Préréglages rapides :");
    GtkWidget *btn_sf = gtk_button_new_with_label("🥋 Fightcade SFIII (A,S,D,Z,X,C)");
    GtkWidget *btn_mame = gtk_button_new_with_label("🕹️ MAME Standard");
    GtkWidget *btn_letters = gtk_button_new_with_label("🔤 Lettres A-Z");

    gtk_box_pack_start(GTK_BOX(preset_box), preset_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(preset_box), btn_sf, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(preset_box), btn_mame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(preset_box), btn_letters, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), preset_box, FALSE, FALSE, 10);

    /* Scrolled window with all 12 keys */
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_add(GTK_CONTAINER(scrolled), grid);

    GtkWidget *key_labels[BTN_IDX_COUNT];

    for (int i = 0; i < BTN_IDX_COUNT; i++) {
        GtkWidget *name_lbl = gtk_label_new(g_btn_names[i]);
        gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);

        key_labels[i] = gtk_label_new(get_key_name(g_custom_keys[i]));
        gtk_widget_set_halign(key_labels[i], GTK_ALIGN_CENTER);

        GtkWidget *change_btn = gtk_button_new_with_label("Modifier");
        g_signal_connect(change_btn, "clicked", G_CALLBACK(on_cfg_row_rebind), GINT_TO_POINTER(i));

        gtk_grid_attach(GTK_GRID(grid), name_lbl, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), key_labels[i], 1, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), change_btn, 2, i, 1, 1);
    }

    /* Handlers for presets inside dialog */
    g_signal_connect_swapped(btn_sf, "clicked", G_CALLBACK(apply_preset_fightcade), NULL);
    g_signal_connect_swapped(btn_sf, "clicked", G_CALLBACK(gtk_widget_destroy), dialog);

    g_signal_connect_swapped(btn_mame, "clicked", G_CALLBACK(apply_preset_mame), NULL);
    g_signal_connect_swapped(btn_mame, "clicked", G_CALLBACK(gtk_widget_destroy), dialog);

    g_signal_connect_swapped(btn_letters, "clicked", G_CALLBACK(apply_preset_letters), NULL);
    g_signal_connect_swapped(btn_letters, "clicked", G_CALLBACK(gtk_widget_destroy), dialog);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void on_open_config_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    open_config_dialog(GTK_WINDOW(g_gui.window));
}

/* GTK GUI Building */
static const char *snes_css =
"window { background-color: #1e1e2e; color: #cdd6f4; font-family: 'Ubuntu', sans-serif; }\n"
".header-box { padding: 12px 16px; background-color: #181825; border-bottom: 1px solid #313244; }\n"
".badge-connected { background-color: #065f46; color: #34d399; font-weight: bold; font-size: 11px; padding: 4px 10px; border-radius: 12px; }\n"
".badge-disconnected { background-color: #7f1d1d; color: #f87171; font-weight: bold; font-size: 11px; padding: 4px 10px; border-radius: 12px; }\n"
".gamepad-frame { background-color: #28283d; border-radius: 28px; padding: 20px; border: 2px solid #45475a; box-shadow: 0 8px 24px rgba(0,0,0,0.4); }\n"
".dpad-btn { background-color: #181825; color: #89b4fa; font-size: 12px; font-weight: bold; min-width: 50px; min-height: 50px; border-radius: 8px; border: 1px solid #313244; }\n"
".action-btn { font-size: 11px; font-weight: bold; min-width: 60px; min-height: 60px; border-radius: 30px; border: 2px solid rgba(0,0,0,0.3); }\n"
".btn-x { background-color: #3b82f6; color: #ffffff; }\n"
".btn-y { background-color: #10b981; color: #ffffff; }\n"
".btn-a { background-color: #ef4444; color: #ffffff; }\n"
".btn-b { background-color: #f59e0b; color: #ffffff; }\n"
".shoulder-btn { background-color: #313244; color: #cdd6f4; font-size: 11px; font-weight: bold; min-width: 140px; min-height: 38px; border-radius: 12px; border: 1px solid #45475a; }\n"
".pill-btn { background-color: #313244; color: #a6adc8; font-size: 10px; font-weight: bold; min-width: 80px; min-height: 28px; border-radius: 10px; border: 1px solid #45475a; }\n"
".cfg-btn { background-color: #45475a; color: #ffffff; font-weight: bold; font-size: 12px; border-radius: 8px; padding: 4px 12px; border: 1px solid #585b70; }\n"
".active-highlight { background-image: none; background-color: #00ff88; color: #000000; border-color: #00ff88; }\n"
".log-bar { padding: 10px 16px; background-color: #181825; border-top: 1px solid #313244; color: #a6adc8; font-size: 12px; }\n"
".mode-combo { background-color: #28283d; color: #cdd6f4; font-weight: bold; border-radius: 8px; }\n";

static void on_switch_toggled(GtkSwitch *widget, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;
    gboolean active = gtk_switch_get_active(widget);
    set_uinput_enabled(&g_backend, active);
}

static void on_mode_changed(GtkComboBox *widget, gpointer user_data) {
    (void)user_data;
    int active = gtk_combo_box_get_active(widget);
    if (active >= 0 && active <= 2) {
        set_emit_mode(&g_backend, (emit_mode_t)active);
    }
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    (void)user_data;
    g_running = 0;
    g_gui_active = false;
    gtk_main_quit();
}

static GtkWidget *create_styled_button(const char *label, const char *class_name, int btn_idx) {
    GtkWidget *btn = gtk_button_new_with_label(label);
    GtkStyleContext *ctx = gtk_widget_get_style_context(btn);
    gtk_style_context_add_class(ctx, class_name);
    if (btn_idx >= 0) {
        g_signal_connect(btn, "clicked", G_CALLBACK(on_pad_button_clicked), GINT_TO_POINTER(btn_idx));
    }
    return btn;
}

static void build_gui(void) {
    g_gui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_gui.window), "SNEScontrol - Panneau de Contrôle");
    gtk_window_set_default_size(GTK_WINDOW(g_gui.window), 740, 560);
    gtk_window_set_position(GTK_WINDOW(g_gui.window), GTK_WIN_POS_CENTER);

    /* Set icon with multiple fallbacks for desktop environment */
    gtk_window_set_default_icon_name("snescontrol");
    if (access("/usr/share/pixmaps/snescontrol.png", R_OK) == 0) {
        gtk_window_set_icon_from_file(GTK_WINDOW(g_gui.window), "/usr/share/pixmaps/snescontrol.png", NULL);
    } else if (access("snescontrol.png", R_OK) == 0) {
        gtk_window_set_icon_from_file(GTK_WINDOW(g_gui.window), "snescontrol.png", NULL);
    }

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, snes_css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(g_gui.window), main_vbox);

    /* Header Bar */
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(header_box), "header-box");

    GtkWidget *title_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title_lbl = gtk_label_new("SNEScontrol");
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    pango_attr_list_insert(attrs, pango_attr_scale_new(1.2));
    gtk_label_set_attributes(GTK_LABEL(title_lbl), attrs);
    pango_attr_list_unref(attrs);
    gtk_widget_set_halign(title_lbl, GTK_ALIGN_START);

    g_gui.device_info_label = gtk_label_new("Recherche de manette...");
    gtk_widget_set_halign(g_gui.device_info_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(title_vbox), title_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(title_vbox), g_gui.device_info_label, FALSE, FALSE, 0);

    /* Remap keys button */
    GtkWidget *cfg_btn = gtk_button_new_with_label("⚙️ Configurer les touches");
    gtk_style_context_add_class(gtk_widget_get_style_context(cfg_btn), "cfg-btn");
    g_signal_connect(cfg_btn, "clicked", G_CALLBACK(on_open_config_clicked), NULL);
    gtk_widget_set_valign(cfg_btn, GTK_ALIGN_CENTER);

    /* Mode selector combo */
    GtkWidget *mode_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *mode_lbl = gtk_label_new("Mode :");
    g_gui.mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_gui.mode_combo), "⌨️ Clavier (FBNeo)");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_gui.mode_combo), "🎮 Manette Virtuelle");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_gui.mode_combo), "⚡ Hybride");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_gui.mode_combo), (int)g_backend.mode);
    g_signal_connect(g_gui.mode_combo, "changed", G_CALLBACK(on_mode_changed), NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_gui.mode_combo), "mode-combo");
    gtk_box_pack_start(GTK_BOX(mode_hbox), mode_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mode_hbox), g_gui.mode_combo, FALSE, FALSE, 0);
    gtk_widget_set_valign(mode_hbox, GTK_ALIGN_CENTER);

    /* Switch toggle for virtual adapter */
    GtkWidget *switch_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *switch_lbl = gtk_label_new("Pilote :");
    g_gui.uinput_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(g_gui.uinput_switch), TRUE);
    g_signal_connect(g_gui.uinput_switch, "notify::active", G_CALLBACK(on_switch_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(switch_hbox), switch_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(switch_hbox), g_gui.uinput_switch, FALSE, FALSE, 0);
    gtk_widget_set_valign(switch_hbox, GTK_ALIGN_CENTER);

    g_gui.status_badge = gtk_label_new("RECHERCHE");
    GtkStyleContext *bctx = gtk_widget_get_style_context(g_gui.status_badge);
    gtk_style_context_add_class(bctx, "badge-disconnected");
    gtk_widget_set_valign(g_gui.status_badge, GTK_ALIGN_CENTER);

    gtk_box_pack_start(GTK_BOX(header_box), title_vbox, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(header_box), cfg_btn, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(header_box), mode_hbox, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(header_box), switch_hbox, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(header_box), g_gui.status_badge, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_vbox), header_box, FALSE, FALSE, 0);

    /* Gamepad Body Frame */
    GtkWidget *center_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(center_container, 20);
    gtk_widget_set_margin_end(center_container, 20);
    gtk_widget_set_margin_top(center_container, 16);
    gtk_widget_set_margin_bottom(center_container, 16);
    gtk_box_pack_start(GTK_BOX(main_vbox), center_container, TRUE, TRUE, 0);

    GtkWidget *gamepad_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_style_context_add_class(gtk_widget_get_style_context(gamepad_frame), "gamepad-frame");
    gtk_box_pack_start(GTK_BOX(center_container), gamepad_frame, TRUE, TRUE, 0);

    /* Shoulders Row: L and R */
    GtkWidget *shoulder_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g_gui.buttons[BTN_IDX_L] = create_styled_button("GÂCHETTE L\n[D]", "shoulder-btn", BTN_IDX_L);
    g_gui.buttons[BTN_IDX_R] = create_styled_button("GÂCHETTE R\n[C]", "shoulder-btn", BTN_IDX_R);
    gtk_box_pack_start(GTK_BOX(shoulder_box), g_gui.buttons[BTN_IDX_L], FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(shoulder_box), g_gui.buttons[BTN_IDX_R], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gamepad_frame), shoulder_box, FALSE, FALSE, 0);

    /* Main Pad Layout (3 columns: D-Pad, Select/Start, Action Buttons) */
    GtkWidget *pad_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(gamepad_frame), pad_hbox, TRUE, TRUE, 0);

    /* Column 1: D-PAD Grid */
    GtkWidget *dpad_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(dpad_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(dpad_grid), 4);

    g_gui.buttons[BTN_IDX_UP] = create_styled_button("▲", "dpad-btn", BTN_IDX_UP);
    g_gui.buttons[BTN_IDX_LEFT] = create_styled_button("◄", "dpad-btn", BTN_IDX_LEFT);
    g_gui.buttons[BTN_IDX_RIGHT] = create_styled_button("►", "dpad-btn", BTN_IDX_RIGHT);
    g_gui.buttons[BTN_IDX_DOWN] = create_styled_button("▼", "dpad-btn", BTN_IDX_DOWN);

    GtkWidget *dpad_center = gtk_label_new("•");
    gtk_style_context_add_class(gtk_widget_get_style_context(dpad_center), "dpad-btn");
    gtk_widget_set_sensitive(dpad_center, FALSE);

    gtk_grid_attach(GTK_GRID(dpad_grid), g_gui.buttons[BTN_IDX_UP], 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(dpad_grid), g_gui.buttons[BTN_IDX_LEFT], 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(dpad_grid), dpad_center, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(dpad_grid), g_gui.buttons[BTN_IDX_RIGHT], 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(dpad_grid), g_gui.buttons[BTN_IDX_DOWN], 1, 2, 1, 1);

    gtk_widget_set_valign(dpad_grid, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(pad_hbox), dpad_grid, TRUE, FALSE, 0);

    /* Column 2: Center SELECT and START */
    GtkWidget *center_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *pill_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    g_gui.buttons[BTN_IDX_SELECT] = create_styled_button("SELECT\n[5]", "pill-btn", BTN_IDX_SELECT);
    g_gui.buttons[BTN_IDX_START] = create_styled_button("START\n[1]", "pill-btn", BTN_IDX_START);
    gtk_box_pack_start(GTK_BOX(pill_hbox), g_gui.buttons[BTN_IDX_SELECT], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pill_hbox), g_gui.buttons[BTN_IDX_START], FALSE, FALSE, 0);

    GtkWidget *snes_logo = gtk_label_new("Nintendo SNES");
    PangoAttrList *lattrs = pango_attr_list_new();
    pango_attr_list_insert(lattrs, pango_attr_scale_new(0.9));
    gtk_label_set_attributes(GTK_LABEL(snes_logo), lattrs);
    pango_attr_list_unref(lattrs);

    gtk_box_pack_start(GTK_BOX(center_vbox), snes_logo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(center_vbox), pill_hbox, FALSE, FALSE, 0);
    gtk_widget_set_valign(center_vbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(pad_hbox), center_vbox, TRUE, FALSE, 0);

    /* Column 3: Action Buttons (Diamond: X Top, Y Left, B Bottom, A Right) */
    GtkWidget *action_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(action_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(action_grid), 4);

    g_gui.buttons[BTN_IDX_X] = create_styled_button("X\n[S]", "action-btn btn-x", BTN_IDX_X);
    g_gui.buttons[BTN_IDX_Y] = create_styled_button("Y\n[A]", "action-btn btn-y", BTN_IDX_Y);
    g_gui.buttons[BTN_IDX_A] = create_styled_button("A\n[X]", "action-btn btn-a", BTN_IDX_A);
    g_gui.buttons[BTN_IDX_B] = create_styled_button("B\n[Z]", "action-btn btn-b", BTN_IDX_B);

    gtk_grid_attach(GTK_GRID(action_grid), g_gui.buttons[BTN_IDX_X], 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(action_grid), g_gui.buttons[BTN_IDX_Y], 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(action_grid), g_gui.buttons[BTN_IDX_A], 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(action_grid), g_gui.buttons[BTN_IDX_B], 1, 2, 1, 1);

    gtk_widget_set_valign(action_grid, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(pad_hbox), action_grid, TRUE, FALSE, 0);

    /* Bottom Event Log Bar */
    g_gui.event_log_label = gtk_label_new("Cliquez sur n'importe quel bouton pour remapper sa touche, ou utilisez la manette physique.");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_gui.event_log_label), "log-bar");
    gtk_widget_set_halign(g_gui.event_log_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(main_vbox), g_gui.event_log_label, FALSE, FALSE, 0);

    update_all_button_labels();

    g_signal_connect(g_gui.window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    gtk_widget_show_all(g_gui.window);
}

/* Headless terminal test mode */
static int run_terminal_test(void) {
    char dev_path[256] = {0};
    uint16_t vendor = 0, product = 0;
    char name[256] = {0};

    printf("[*] Recherche de la manette SNES...\n");
    if (!find_gamepad(dev_path, sizeof(dev_path), &vendor, &product, name, sizeof(name))) {
        fprintf(stderr, "[-] Aucune manette détectée. Veuillez la brancher en USB.\n");
        return 1;
    }

    printf("[+] Connecté à : %s (%04x:%04x) sur %s\n", name, vendor, product, dev_path);
    printf("=================================================================\n");
    printf("   SNES Controller Live Test Mode (Appuyez sur Ctrl+C pour quitter)\n");
    printf("=================================================================\n\n");

    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        perror("[-] Erreur ouverture périphérique");
        return 1;
    }

    bool up = false, down = false, left = false, right = false;
    struct input_event ev;

    while (g_running) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n < (ssize_t)sizeof(ev)) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            printf("\n[-] Manette déconnectée.\n");
            break;
        }

        if (ev.type == EV_KEY) {
            int idx = map_physical_to_idx(ev.code);
            if (idx >= 0) {
                const char *bname = g_btn_names[idx];
                const char *kname = get_key_name(g_custom_keys[idx]);
                printf(" [BOUTON] Code brut %3d -> %-14s (Touche: %-10s) : %s\n",
                       ev.code, bname, kname, ev.value ? "\033[1;32m[PRESSÉ]\033[0m" : "\033[0;37m[RELÂCHÉ]\033[0m");
                fflush(stdout);
            }
        } else if (ev.type == EV_ABS) {
            if (ev.code == ABS_X) {
                bool new_left = (ev.value < AXIS_MIN_THRESHOLD);
                bool new_right = (ev.value > AXIS_MAX_THRESHOLD);
                if (new_left != left) {
                    left = new_left;
                    printf(" [CROIX ] Axe X=%3d  -> GAUCHE [%s] : %s\n",
                           ev.value, get_key_name(g_custom_keys[BTN_IDX_LEFT]), left ? "\033[1;32m[PRESSÉ]\033[0m" : "\033[0;37m[RELÂCHÉ]\033[0m");
                }
                if (new_right != right) {
                    right = new_right;
                    printf(" [CROIX ] Axe X=%3d  -> DROITE [%s] : %s\n",
                           ev.value, get_key_name(g_custom_keys[BTN_IDX_RIGHT]), right ? "\033[1;32m[PRESSÉ]\033[0m" : "\033[0;37m[RELÂCHÉ]\033[0m");
                }
                fflush(stdout);
            } else if (ev.code == ABS_Y) {
                bool new_up = (ev.value < AXIS_MIN_THRESHOLD);
                bool new_down = (ev.value > AXIS_MAX_THRESHOLD);
                if (new_up != up) {
                    up = new_up;
                    printf(" [CROIX ] Axe Y=%3d  -> HAUT   [%s] : %s\n",
                           ev.value, get_key_name(g_custom_keys[BTN_IDX_UP]), up ? "\033[1;32m[PRESSÉ]\033[0m" : "\033[0;37m[RELÂCHÉ]\033[0m");
                }
                if (new_down != down) {
                    down = new_down;
                    printf(" [CROIX ] Axe Y=%3d  -> BAS    [%s] : %s\n",
                           ev.value, get_key_name(g_custom_keys[BTN_IDX_DOWN]), down ? "\033[1;32m[PRESSÉ]\033[0m" : "\033[0;37m[RELÂCHÉ]\033[0m");
                }
                fflush(stdout);
            }
        }
    }

    close(fd);
    printf("\n[*] Test terminé.\n");
    return 0;
}

int main(int argc, char **argv) {
    bool cli_mode = false;
    bool daemon_mode = false;
    bool test_mode = false;
    emit_mode_t default_mode = MODE_KEYBOARD; /* Default to FBNeo keyboard arcade */

    load_key_mapping();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--cli") == 0) {
            cli_mode = true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = true;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
            test_mode = true;
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--mode") == 0) {
            if (i + 1 < argc) {
                i++;
                if (strcmp(argv[i], "keyboard") == 0) default_mode = MODE_KEYBOARD;
                else if (strcmp(argv[i], "gamepad") == 0) default_mode = MODE_GAMEPAD;
                else if (strcmp(argv[i], "hybrid") == 0) default_mode = MODE_HYBRID;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n\n", argv[0]);
            printf("Options:\n");
            printf("  (aucun)               Lancer l'application graphique GTK3\n");
            printf("  -c, --cli             Lancer en mode console sans interface\n");
            printf("  -d, --daemon          Lancer en tâche de fond (démon d'arrière-plan)\n");
            printf("  -m, --mode [MODE]     keyboard | gamepad | hybrid (par défaut: keyboard)\n");
            printf("  -t, --test            Mode test interactif dans la console\n");
            printf("  -h, --help            Afficher cette aide\n");
            return 0;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    if (test_mode) {
        return run_terminal_test();
    }

    memset(&g_backend, 0, sizeof(g_backend));
    pthread_mutex_init(&g_backend.lock, NULL);
    g_backend.dev_fd = -1;
    g_backend.uinput_fd = -1;
    g_backend.uinput_enabled = true;
    g_backend.mode = default_mode;

    if (daemon_mode) {
        if (daemon(0, 0) < 0) {
            perror("[-] Échec daemonize");
            return 1;
        }
        cli_mode = true;
    }

    if (!cli_mode) {
        /* Launch GTK GUI */
        gtk_init(&argc, &argv);
        g_gui_active = true;
        build_gui();

        if (pthread_create(&g_worker_thread, NULL, worker_thread_func, NULL) != 0) {
            fprintf(stderr, "[-] Impossible de démarrer le thread d'écoute.\n");
            return 1;
        }

        gtk_main();

        g_running = 0;
        pthread_join(g_worker_thread, NULL);
        pthread_mutex_destroy(&g_backend.lock);
        return 0;
    }

    /* CLI Mode */
    printf("[*] Démarrage de snescontrol en mode autonome/démon...\n");
    worker_thread_func(NULL);
    pthread_mutex_destroy(&g_backend.lock);
    return 0;
}
