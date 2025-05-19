#include <gtk/gtk.h>
#include <rfb/rfbclient.h>

#define rfbWheelLeftMask 32
#define rfbWheelRightMask 64

typedef struct {
    GtkWidget *area;
    GdkSurface *surface;
} Window;

typedef struct {
    gboolean inhibited;
} Status;

typedef struct {
    gint buttons;
    gdouble X, Y;
} Mouse;

typedef struct {
    GdkClipboard *cb;
    gchar *text;
} Clipboard;

typedef struct {
    Status *status;
    Window *window;
    Mouse *mouse;
    Clipboard *clipboard;
} Data;

static void init_data(Data *data) {
    data->status = g_new0(Status, 1);
    data->window = g_new0(Window, 1);
    data->mouse = g_new0(Mouse, 1);
    data->clipboard = g_new0(Clipboard, 1);
}

static void clean_data(Data *data) {
    g_clear_pointer(&data->status, g_free);
    g_clear_pointer(&data->window, g_free);
    g_clear_pointer(&data->mouse, g_free);
    g_clear_pointer(&data->clipboard->text, g_free);
    g_clear_pointer(&data->clipboard, g_free);

    g_clear_pointer(&data, g_free);
}

static void inhibit_system_shortcuts(GdkSurface *surface) {
    if (GDK_IS_TOPLEVEL(surface)) {
        gdk_toplevel_inhibit_system_shortcuts(GDK_TOPLEVEL(surface), NULL);
    }
}

static void restore_system_shortcuts(GdkSurface *surface) {
    if (GDK_IS_TOPLEVEL(surface)) {
        gdk_toplevel_restore_system_shortcuts(GDK_TOPLEVEL(surface));
    }
}

static void send_clipboard_content_callback(GdkClipboard *clipboard, GAsyncResult *result, rfbClient *client) {
    gchar *text = gdk_clipboard_read_text_finish(clipboard, result, NULL);
    if (text) {
        SendClientCutText(client, text, strlen(text));
        g_free(text);
    }
}

static void send_clipboard_content(rfbClient *client) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    if (GDK_IS_CLIPBOARD(data->clipboard->cb)) {
        gdk_clipboard_read_text_async(data->clipboard->cb, NULL, (GAsyncReadyCallback)send_clipboard_content_callback, client);
    }
}

static void write_clipboard_content(rfbClient *client) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    if (GDK_IS_CLIPBOARD(data->clipboard->cb)) {
        if (data->clipboard->text) {
            gdk_clipboard_set_text(data->clipboard->cb, data->clipboard->text);
        }
    }
}

static void handle_key_press(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, rfbClient *client) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    if (state == GDK_NO_MODIFIER_MASK && keyval == GDK_KEY_Pause) {
        if (data->status->inhibited) {
            write_clipboard_content(client);
        } else {
            send_clipboard_content(client);
        }
        SendIncrementalFramebufferUpdateRequest(client);
    } else {
        SendKeyEvent(client, keyval, TRUE);
    }
}

static void handle_key_release(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, rfbClient *client) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    if (state == GDK_NO_MODIFIER_MASK && keyval == GDK_KEY_Pause) {
        if (data->status->inhibited) {
            restore_system_shortcuts(data->window->surface);
        } else {
            inhibit_system_shortcuts(data->window->surface);
        }
    } else {
        SendKeyEvent(client, keyval, FALSE);
    }
}

static void process_mouse_button(GtkGestureClick *gesture, gboolean pressed, rfbClient *client) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    gint mask = 0;

    switch (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture))) {
        case GDK_BUTTON_PRIMARY:
            mask = rfbButton1Mask;
            break;
        case GDK_BUTTON_MIDDLE:
            mask = rfbButton2Mask;
            break;
        case GDK_BUTTON_SECONDARY:
            mask = rfbButton3Mask;
            break;
        default:
            g_warning("Unknown BUTTON");
            return;
    }

    if (pressed) {
        /* if unrelease, release first */
        if (data->mouse->buttons & mask) {
            data->mouse->buttons &= ~mask;
            SendPointerEvent(client, data->mouse->X, data->mouse->Y, data->mouse->buttons);
        }
        data->mouse->buttons |= mask;
    } else {
        /* if unpress, press first*/
        if (!(data->mouse->buttons & mask)) {
            data->mouse->buttons |= mask;
            SendPointerEvent(client, data->mouse->X, data->mouse->Y, data->mouse->buttons);
        }
        data->mouse->buttons &= ~mask;
    }

    SendPointerEvent(client, data->mouse->X, data->mouse->Y, data->mouse->buttons);
}

static void handle_mouse_press(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, rfbClient *client) {
    process_mouse_button(gesture, TRUE, client);
}

static void handle_mouse_release(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, rfbClient *client) {
    process_mouse_button(gesture, FALSE, client);
}

static void handle_mouse_motion(GtkEventControllerMotion *controller, gdouble x, gdouble y, rfbClient *client) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    if (data->status->inhibited) {
        data->mouse->X = x / ((gdouble)gtk_widget_get_width(data->window->area) / client->width);
        data->mouse->Y = y / ((gdouble)gtk_widget_get_height(data->window->area) / client->height);
        SendPointerEvent(client, data->mouse->X, data->mouse->Y, data->mouse->buttons);
    }
}

static void handle_scroll(GtkEventControllerScroll *controller, gdouble dx, gdouble dy, rfbClient *client) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    if (data->status->inhibited) {
        gint mask = 0;
        if (dy < 0) {
            mask |= rfbWheelUpMask; // Scroll up
        } else if (dy > 0) {
            mask |= rfbWheelDownMask; // Scroll down
        }
        if (dx < 0) {
            mask |= rfbWheelLeftMask; // Scroll left
        } else if (dx > 0) {
            mask |= rfbWheelRightMask; // Scroll right
        }
        data->mouse->buttons |= mask;
        SendPointerEvent(client, data->mouse->X, data->mouse->Y, data->mouse->buttons);
        data->mouse->buttons &= ~mask;
        SendPointerEvent(client, data->mouse->X, data->mouse->Y, data->mouse->buttons);
    }
}

static void handle_clipboard_content(rfbClient *client, const char *text, int textlen) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    if (text) {
        g_clear_pointer(&data->clipboard->text, g_free);
        data->clipboard->text = g_strndup(text, textlen);
    }
}

static void handle_framebuffer_update(rfbClient *client, int x, int y, int w, int h) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");

    if (client->frameBuffer) {
        GBytes *bytes = g_bytes_new_static(
                client->frameBuffer,
                client->width * client->height * 4
            );
        GdkPaintable *texture = GDK_PAINTABLE(gdk_memory_texture_new(
                client->width,
                client->height,
                GDK_MEMORY_R8G8B8A8,
                bytes,
                client->width * 4
            ));

        gtk_picture_set_paintable(GTK_PICTURE(data->window->area), texture);

        g_object_unref(texture);
        g_bytes_unref(bytes);
    }
}

static gboolean handle_vnc_message(rfbClient *client) {
    if (WaitForMessage(client, 500) > 0) {
        if (!HandleRFBServerMessage(client)) {
            rfbClientErr("Something wrong, exit\n");
            g_idle_add((GSourceFunc)g_application_quit, g_application_get_default());
            return G_SOURCE_REMOVE;
        }
    }

    return G_SOURCE_CONTINUE;
}

static void on_shortcuts_inhibited_notify(GdkSurface *surface, GParamSpec *pspec, Data *data) {
    g_object_get(GDK_TOPLEVEL(surface), "shortcuts-inhibited", &data->status->inhibited, NULL);
}

static void on_map(GtkWidget *window, rfbClient *client) {
    Data *data = (Data *)rfbClientGetClientData(client, "Data");
    data->window->surface = gtk_native_get_surface(GTK_NATIVE(window));
    data->clipboard->cb = gdk_display_get_clipboard(gdk_display_get_default());

    g_timeout_add(10, (GSourceFunc)handle_vnc_message, client);

    g_signal_connect(GDK_TOPLEVEL(data->window->surface), "notify::shortcuts-inhibited", G_CALLBACK(on_shortcuts_inhibited_notify), data);

    gtk_window_fullscreen(GTK_WINDOW(window));
}

static void on_activate(GtkApplication *app) {
    rfbClient *client = g_object_get_data(G_OBJECT(app), "rfb-client");
    Data *data = (Data *)rfbClientGetClientData(client, "Data");

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "VNC");

    data->window->area = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(data->window->area), GTK_CONTENT_FIT_FILL);

    gtk_widget_set_cursor(data->window->area, gdk_cursor_new_from_name("none", NULL));
    gtk_window_set_child(GTK_WINDOW(window), data->window->area);

    GtkEventController *motion_controller = gtk_event_controller_motion_new();
    g_signal_connect(motion_controller, "motion", G_CALLBACK(handle_mouse_motion), client);
    gtk_widget_add_controller(GTK_WIDGET(data->window->area), motion_controller);

    GtkGesture *click_gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), 0);
    g_signal_connect(click_gesture, "pressed", G_CALLBACK(handle_mouse_press), client);
    g_signal_connect(click_gesture, "released", G_CALLBACK(handle_mouse_release), client);
    gtk_widget_add_controller(GTK_WIDGET(data->window->area), GTK_EVENT_CONTROLLER(click_gesture));

    GtkEventController *scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(handle_scroll), client);
    gtk_widget_add_controller(GTK_WIDGET(data->window->area), scroll_controller);

    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed", G_CALLBACK(handle_key_press), client);
    g_signal_connect(key_controller, "key-released", G_CALLBACK(handle_key_release), client);
    gtk_widget_add_controller(GTK_WIDGET(window), key_controller);

    g_signal_connect(window, "map", G_CALLBACK(on_map), client);

    gtk_window_present(GTK_WINDOW(window));
}

static gint on_startup(GApplication *app, GApplicationCommandLine *cmdline) {
    gint argc;
    gchar **argv = g_application_command_line_get_arguments(cmdline, &argc);

    rfbClient *client = rfbGetClient(8, 4, 4);
    client->appData.compressLevel = 0;
    client->appData.qualityLevel = 9;
    client->appData.useRemoteCursor = TRUE;
    client->programName = "VncClient";
    client->canHandleNewFBSize = FALSE;
    // client->format.bigEndian = FALSE;
    // client->format.redShift = 16;
    // client->format.greenShift = 8;
    // client->format.blueShift = 0;
    // client->appData.enableJPEG = FALSE;

    client->GotXCutText = handle_clipboard_content;
    client->GotFrameBufferUpdate = handle_framebuffer_update;

    if (rfbInitClient(client, &argc, argv)) {
        SetFormatAndEncodings(client);

        Data *data = g_new0(Data, 1);
        init_data(data);
        rfbClientSetClientData(client, "Data", data);

        g_object_set_data(G_OBJECT(app), "rfb-client", client);
        g_application_command_line_set_exit_status(cmdline, EXIT_SUCCESS);
        g_application_activate(app);
    } else {
        g_object_set_data(G_OBJECT(app), "rfb-client", NULL);
        g_application_command_line_set_exit_status(cmdline, EXIT_FAILURE);
    }

    return g_application_command_line_get_exit_status(cmdline);
}

static void on_cleanup(GApplication *app) {
    rfbClient *client = g_object_get_data(G_OBJECT(app), "rfb-client");
    if (client) {
        Data *data = (Data *)rfbClientGetClientData(client, "Data");
        clean_data(data);
        rfbClientCleanup(client);
    }
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("org.local.VncClient", G_APPLICATION_HANDLES_COMMAND_LINE);

    g_signal_connect(app, "command-line", G_CALLBACK(on_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_cleanup), NULL);

    gint status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);

    return status;
}
