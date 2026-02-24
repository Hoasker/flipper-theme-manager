#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <gui/modules/widget.h>
#include <storage/storage.h>

#define TAG "ThemeManager"

#define ANIMATION_PACKS_PATH EXT_PATH("animation_packs")
#define DOLPHIN_PATH         EXT_PATH("dolphin")
#define MANIFEST_FILENAME    "manifest.txt"
#define META_FILENAME        "meta.txt"
#define ANIMS_DIRNAME        "Anims"
#define DOLPHIN_MANIFEST     DOLPHIN_PATH "/" MANIFEST_FILENAME
#define DOLPHIN_BACKUP_PATH  EXT_PATH("dolphin_backup")
#define MANIFEST_HEADER      "Filetype: Flipper Animation Manifest"

#define MAX_THEMES    64
#define MAX_NAME_LEN  64
#define MAX_LABEL_LEN 32

#define MENU_INDEX_RESTORE (MAX_THEMES + 1)


typedef enum {
    ThemeTypePack,       
    ThemeTypeAnimsPack,  
    ThemeTypeSingle,    
} ThemeType;


typedef enum {
    ThemeManagerViewSubmenu,
    ThemeManagerViewInfo,
    ThemeManagerViewConfirm,
    ThemeManagerViewReboot,
    ThemeManagerViewDeleteConfirm,
    ThemeManagerViewPopup,
    ThemeManagerViewLoading,
} ThemeManagerView;


typedef struct {
    Storage* storage;
    Gui* gui;

    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* info_widget;
    DialogEx* confirm_dialog;
    DialogEx* reboot_dialog;
    DialogEx* delete_dialog;
    Popup* popup;
    Loading* loading;

    char theme_names[MAX_THEMES][MAX_NAME_LEN];
    char menu_labels[MAX_THEMES][MAX_LABEL_LEN];
    ThemeType theme_types[MAX_THEMES];
    uint32_t theme_count;
    uint32_t selected_index;
    bool has_backup;

    FuriString* dialog_text;
} ThemeManagerApp;


static void theme_manager_scan_themes(ThemeManagerApp* app);
static bool theme_manager_apply_pack(ThemeManagerApp* app, const char* merge_src_dir);
static bool theme_manager_apply_single(ThemeManagerApp* app, const char* theme_name);
static bool theme_manager_restore_backup(ThemeManagerApp* app);
static bool theme_manager_backup_dolphin(ThemeManagerApp* app);
static bool theme_manager_parse_manifest(
    ThemeManagerApp* app, const char* path, uint32_t* out_count);

static void theme_manager_submenu_callback(void* context, uint32_t index);
static void theme_manager_confirm_callback(DialogExResult result, void* context);
static void theme_manager_reboot_callback(DialogExResult result, void* context);
static void theme_manager_delete_callback(DialogExResult result, void* context);
static void theme_manager_info_button_callback(
    GuiButtonType button, InputType input_type, void* context);
static void theme_manager_popup_callback(void* context);
static void theme_manager_show_error(ThemeManagerApp* app, const char* message);
static void theme_manager_show_info(ThemeManagerApp* app, uint32_t index);
static bool theme_manager_delete_theme(ThemeManagerApp* app, uint32_t index);
static void theme_manager_populate_submenu(ThemeManagerApp* app);

static uint32_t theme_manager_nav_exit(void* context);
static uint32_t theme_manager_nav_submenu(void* context);
static uint32_t theme_manager_nav_info(void* context);

// -------------------------------------------------------------------
// Parse manifest.txt — validate header and count "Name:" entries
// Returns true if manifest is valid, writes animation count to *out_count
// -------------------------------------------------------------------
static bool theme_manager_parse_manifest(
    ThemeManagerApp* app, const char* path, uint32_t* out_count) {
    *out_count = 0;

    File* file = storage_file_alloc(app->storage);
    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        return false;
    }

    FuriString* accum = furi_string_alloc();
    char buf[128];
    uint16_t bytes_read;

    while((bytes_read = storage_file_read(file, buf, sizeof(buf) - 1)) > 0) {
        buf[bytes_read] = '\0';
        furi_string_cat_str(accum, buf);
    }

    storage_file_close(file);
    storage_file_free(file);

    const char* str = furi_string_get_cstr(accum);

    if(strstr(str, MANIFEST_HEADER) == NULL) {
        furi_string_free(accum);
        return false;
    }

    const char* ptr = str;
    while((ptr = strstr(ptr, "Name:")) != NULL) {
        if(ptr == str || *(ptr - 1) == '\n') {
            (*out_count)++;
        }
        ptr += 5;
    }

    furi_string_free(accum);
    return true;
}

// -------------------------------------------------------------------
// Calculate total size of a directory (recursive)
// -------------------------------------------------------------------
static uint64_t theme_manager_get_dir_size(ThemeManagerApp* app, const char* path) {
    uint64_t total = 0;
    File* dir = storage_file_alloc(app->storage);

    if(!storage_dir_open(dir, path)) {
        storage_file_free(dir);
        return 0;
    }

    FileInfo file_info;
    char name[MAX_NAME_LEN];
    FuriString* child_path = furi_string_alloc();

    while(storage_dir_read(dir, &file_info, name, sizeof(name))) {
        furi_string_printf(child_path, "%s/%s", path, name);
        if(file_info.flags & FSF_DIRECTORY) {
            total += theme_manager_get_dir_size(app, furi_string_get_cstr(child_path));
        } else {
            total += file_info.size;
        }
    }

    furi_string_free(child_path);
    storage_dir_close(dir);
    storage_file_free(dir);

    return total;
}

// -------------------------------------------------------------------
// Scan /ext/animation_packs/ for all 3 formats
// -------------------------------------------------------------------
static void theme_manager_scan_themes(ThemeManagerApp* app) {
    app->theme_count = 0;
    app->has_backup = storage_dir_exists(app->storage, DOLPHIN_BACKUP_PATH);

    if(!storage_dir_exists(app->storage, ANIMATION_PACKS_PATH)) {
        FURI_LOG_W(TAG, "Directory %s not found", ANIMATION_PACKS_PATH);
        return;
    }

    File* dir = storage_file_alloc(app->storage);

    if(!storage_dir_open(dir, ANIMATION_PACKS_PATH)) {
        FURI_LOG_E(TAG, "Failed to open %s", ANIMATION_PACKS_PATH);
        storage_file_free(dir);
        return;
    }

    FileInfo file_info;
    char name[MAX_NAME_LEN];

    FuriString* check_path = furi_string_alloc();

    while(app->theme_count < MAX_THEMES &&
          storage_dir_read(dir, &file_info, name, sizeof(name))) {
        if(!(file_info.flags & FSF_DIRECTORY)) continue;

        ThemeType detected_type;
        bool found = false;

        furi_string_printf(check_path, "%s/%s/%s",
            ANIMATION_PACKS_PATH, name, MANIFEST_FILENAME);
        if(storage_file_exists(app->storage, furi_string_get_cstr(check_path))) {
            detected_type = ThemeTypePack;
            found = true;
            FURI_LOG_I(TAG, "[Pack] %s", name);
        }

        if(!found) {
            furi_string_printf(check_path, "%s/%s/%s/%s",
                ANIMATION_PACKS_PATH, name, ANIMS_DIRNAME, MANIFEST_FILENAME);
            if(storage_file_exists(app->storage, furi_string_get_cstr(check_path))) {
                detected_type = ThemeTypeAnimsPack;
                found = true;
                FURI_LOG_I(TAG, "[AnimsPack] %s", name);
            }
        }

        if(!found) {
            furi_string_printf(check_path, "%s/%s/%s",
                ANIMATION_PACKS_PATH, name, META_FILENAME);
            if(storage_file_exists(app->storage, furi_string_get_cstr(check_path))) {
                detected_type = ThemeTypeSingle;
                found = true;
                FURI_LOG_I(TAG, "[Single] %s", name);
            }
        }

        if(found) {
            strncpy(app->theme_names[app->theme_count], name, MAX_NAME_LEN - 1);
            app->theme_names[app->theme_count][MAX_NAME_LEN - 1] = '\0';
            app->theme_types[app->theme_count] = detected_type;
            app->theme_count++;
        } else {
            FURI_LOG_W(TAG, "Skipping %s (unknown format)", name);
        }
    }

    furi_string_free(check_path);

    storage_dir_close(dir);
    storage_file_free(dir);

    FURI_LOG_I(TAG, "Total: %lu themes, backup: %s",
        app->theme_count, app->has_backup ? "yes" : "no");
}

// -------------------------------------------------------------------
// Backup entire /ext/dolphin/ → /ext/dolphin_backup/
// Uses rename (fast on FAT32 — just a metadata change)
// -------------------------------------------------------------------
static bool theme_manager_backup_dolphin(ThemeManagerApp* app) {
    if(!storage_dir_exists(app->storage, DOLPHIN_PATH)) {
        return true;
    }

    if(storage_dir_exists(app->storage, DOLPHIN_BACKUP_PATH)) {
        storage_simply_remove_recursive(app->storage, DOLPHIN_BACKUP_PATH);
    }

    FS_Error err = storage_common_rename(app->storage, DOLPHIN_PATH, DOLPHIN_BACKUP_PATH);
    if(err != FSE_OK) {
        FURI_LOG_E(TAG, "Backup rename failed (err %d)", err);
        return false;
    }

    app->has_backup = true;
    FURI_LOG_I(TAG, "Backed up /ext/dolphin/ -> /ext/dolphin_backup/");
    return true;
}

// -------------------------------------------------------------------
// Apply Pack theme (format A or B): merge directory into /ext/dolphin/
// -------------------------------------------------------------------
static bool theme_manager_apply_pack(ThemeManagerApp* app, const char* merge_src_dir) {
    FS_Error err = storage_common_merge(app->storage, merge_src_dir, DOLPHIN_PATH);

    if(err != FSE_OK) {
        FURI_LOG_E(TAG, "Merge failed: %s -> %s (err %d)", merge_src_dir, DOLPHIN_PATH, err);
        return false;
    }

    FURI_LOG_I(TAG, "Merged: %s -> %s", merge_src_dir, DOLPHIN_PATH);
    return true;
}

// -------------------------------------------------------------------
// Apply Single animation (format C):
//   1. Copy animation folder to /ext/dolphin/<name>/
//   2. Generate manifest.txt with single Name: entry
// -------------------------------------------------------------------
static bool theme_manager_apply_single(ThemeManagerApp* app, const char* theme_name) {
    FuriString* src_dir = furi_string_alloc_printf(
        "%s/%s", ANIMATION_PACKS_PATH, theme_name);
    FuriString* dst_dir = furi_string_alloc_printf(
        "%s/%s", DOLPHIN_PATH, theme_name);

    storage_common_mkdir(app->storage, furi_string_get_cstr(dst_dir));

    FS_Error err = storage_common_merge(
        app->storage,
        furi_string_get_cstr(src_dir),
        furi_string_get_cstr(dst_dir));

    furi_string_free(src_dir);
    furi_string_free(dst_dir);

    if(err != FSE_OK) {
        FURI_LOG_E(TAG, "Copy single anim failed (err %d)", err);
        return false;
    }

    File* manifest = storage_file_alloc(app->storage);
    if(!storage_file_open(manifest, DOLPHIN_MANIFEST, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FURI_LOG_E(TAG, "Failed to create manifest");
        storage_file_free(manifest);
        return false;
    }

    FuriString* content = furi_string_alloc_printf(
        "Filetype: Flipper Animation Manifest\n"
        "Version: 1\n"
        "\n"
        "Name: %s\n"
        "Min butthurt: 0\n"
        "Max butthurt: 14\n"
        "Min level: 1\n"
        "Max level: 30\n"
        "Weight: 5\n",
        theme_name);

    const char* str = furi_string_get_cstr(content);
    uint16_t len = strlen(str);
    uint16_t written = storage_file_write(manifest, str, len);

    if(written != len) {
        FURI_LOG_E(TAG, "Manifest write incomplete (%u/%u bytes)", written, len);
        furi_string_free(content);
        storage_file_close(manifest);
        storage_file_free(manifest);
        return false;
    }

    furi_string_free(content);
    storage_file_close(manifest);
    storage_file_free(manifest);

    FURI_LOG_I(TAG, "Applied single animation: %s (manifest generated)", theme_name);
    return true;
}

// -------------------------------------------------------------------
// Main apply dispatcher — routes to correct handler based on type
// -------------------------------------------------------------------
static bool theme_manager_apply_theme(ThemeManagerApp* app, uint32_t index) {
    if(index >= app->theme_count) return false;

    if(!theme_manager_backup_dolphin(app)) {
        FURI_LOG_E(TAG, "Backup failed, aborting apply");
        return false;
    }

    storage_common_mkdir(app->storage, DOLPHIN_PATH);

    const char* name = app->theme_names[index];
    ThemeType type = app->theme_types[index];

    FuriString* src = furi_string_alloc();
    bool success = false;

    switch(type) {
    case ThemeTypePack:
        furi_string_printf(src, "%s/%s", ANIMATION_PACKS_PATH, name);
        success = theme_manager_apply_pack(app, furi_string_get_cstr(src));
        break;

    case ThemeTypeAnimsPack:
        furi_string_printf(src, "%s/%s/%s", ANIMATION_PACKS_PATH, name, ANIMS_DIRNAME);
        success = theme_manager_apply_pack(app, furi_string_get_cstr(src));
        break;

    case ThemeTypeSingle:
        success = theme_manager_apply_single(app, name);
        break;
    }

    furi_string_free(src);
    return success;
}

// -------------------------------------------------------------------
// Restore backup: swap /ext/dolphin_backup/ → /ext/dolphin/
// -------------------------------------------------------------------
static bool theme_manager_restore_backup(ThemeManagerApp* app) {
    if(!storage_dir_exists(app->storage, DOLPHIN_BACKUP_PATH)) {
        return false;
    }

    if(storage_dir_exists(app->storage, DOLPHIN_PATH)) {
        storage_simply_remove_recursive(app->storage, DOLPHIN_PATH);
    }

    FS_Error err = storage_common_rename(app->storage, DOLPHIN_BACKUP_PATH, DOLPHIN_PATH);
    if(err != FSE_OK) {
        FURI_LOG_E(TAG, "Restore rename failed (err %d)", err);
        return false;
    }

    app->has_backup = false;
    FURI_LOG_I(TAG, "Restored /ext/dolphin_backup/ -> /ext/dolphin/");
    return true;
}

// -------------------------------------------------------------------
// Delete theme from SD card
// -------------------------------------------------------------------
static bool theme_manager_delete_theme(ThemeManagerApp* app, uint32_t index) {
    if(index >= app->theme_count) return false;

    FuriString* theme_path = furi_string_alloc_printf(
        "%s/%s", ANIMATION_PACKS_PATH, app->theme_names[index]);

    bool success = storage_simply_remove_recursive(
        app->storage, furi_string_get_cstr(theme_path));

    if(success) {
        FURI_LOG_I(TAG, "Deleted theme: %s", app->theme_names[index]);
    } else {
        FURI_LOG_E(TAG, "Failed to delete: %s", app->theme_names[index]);
    }

    furi_string_free(theme_path);
    return success;
}

// -------------------------------------------------------------------
// Show theme info screen (Widget view)
// -------------------------------------------------------------------
static void theme_manager_show_info(ThemeManagerApp* app, uint32_t index) {
    if(index >= app->theme_count) return;

    app->selected_index = index;
    widget_reset(app->info_widget);

    const char* name = app->theme_names[index];
    ThemeType type = app->theme_types[index];

    const char* type_label;
    uint32_t anim_count = 0;

    switch(type) {
    case ThemeTypePack: {
        type_label = "Pack";
        FuriString* mpath = furi_string_alloc_printf(
            "%s/%s/%s", ANIMATION_PACKS_PATH, name, MANIFEST_FILENAME);
        theme_manager_parse_manifest(app, furi_string_get_cstr(mpath), &anim_count);
        furi_string_free(mpath);
        break;
    }
    case ThemeTypeAnimsPack: {
        type_label = "Anim Pack";
        FuriString* mpath = furi_string_alloc_printf(
            "%s/%s/%s/%s", ANIMATION_PACKS_PATH, name, ANIMS_DIRNAME, MANIFEST_FILENAME);
        theme_manager_parse_manifest(app, furi_string_get_cstr(mpath), &anim_count);
        furi_string_free(mpath);
        break;
    }
    case ThemeTypeSingle:
        type_label = "Single";
        anim_count = 1;
        break;
    default:
        type_label = "Unknown";
        break;
    }

    FuriString* theme_dir = furi_string_alloc_printf(
        "%s/%s", ANIMATION_PACKS_PATH, name);
    uint64_t size_bytes = theme_manager_get_dir_size(app, furi_string_get_cstr(theme_dir));
    furi_string_free(theme_dir);

    char size_str[16];
    if(size_bytes >= 1024 * 1024) {
        snprintf(size_str, sizeof(size_str), "%lu.%lu MB",
            (uint32_t)(size_bytes / (1024 * 1024)),
            (uint32_t)((size_bytes % (1024 * 1024)) * 10 / (1024 * 1024)));
    } else if(size_bytes >= 1024) {
        snprintf(size_str, sizeof(size_str), "%lu KB", (uint32_t)(size_bytes / 1024));
    } else {
        snprintf(size_str, sizeof(size_str), "%lu B", (uint32_t)size_bytes);
    }

    widget_add_string_element(
        app->info_widget, 64, 2, AlignCenter, AlignTop, FontPrimary, name);

    static char info_line1[48];
    static char info_line2[24];

    snprintf(info_line1, sizeof(info_line1), "Type: %s  Anims: %lu", type_label, anim_count);
    widget_add_string_element(
        app->info_widget, 64, 18, AlignCenter, AlignTop, FontSecondary, info_line1);

    snprintf(info_line2, sizeof(info_line2), "Size: %s", size_str);
    widget_add_string_element(
        app->info_widget, 64, 30, AlignCenter, AlignTop, FontSecondary, info_line2);

    widget_add_button_element(
        app->info_widget, GuiButtonTypeLeft, "Back",
        theme_manager_info_button_callback, app);
    widget_add_button_element(
        app->info_widget, GuiButtonTypeCenter, "Delete",
        theme_manager_info_button_callback, app);
    widget_add_button_element(
        app->info_widget, GuiButtonTypeRight, "Apply",
        theme_manager_info_button_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewInfo);
}

// -------------------------------------------------------------------
// Widget button callback for Info screen
// -------------------------------------------------------------------
static void theme_manager_info_button_callback(
    GuiButtonType button, InputType input_type, void* context) {
    ThemeManagerApp* app = context;
    if(input_type != InputTypeShort) return;

    if(button == GuiButtonTypeLeft) {
        view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewSubmenu);

    } else if(button == GuiButtonTypeRight) {
        uint32_t index = app->selected_index;
        if(index >= app->theme_count) return;

        dialog_ex_set_header(
            app->confirm_dialog, app->theme_names[index], 64, 0, AlignCenter, AlignTop);

        furi_string_printf(
            app->dialog_text,
            "Apply this theme?\nBackup will be created.");
        dialog_ex_set_text(
            app->confirm_dialog,
            furi_string_get_cstr(app->dialog_text),
            64, 26, AlignCenter, AlignTop);

        dialog_ex_set_left_button_text(app->confirm_dialog, "Back");
        dialog_ex_set_right_button_text(app->confirm_dialog, "Apply");

        view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewConfirm);

    } else if(button == GuiButtonTypeCenter) {
        uint32_t index = app->selected_index;
        if(index >= app->theme_count) return;

        dialog_ex_set_header(
            app->delete_dialog, "Delete Theme?", 64, 0, AlignCenter, AlignTop);

        furi_string_printf(
            app->dialog_text,
            "%s\nThis cannot be undone!",
            app->theme_names[index]);
        dialog_ex_set_text(
            app->delete_dialog,
            furi_string_get_cstr(app->dialog_text),
            64, 26, AlignCenter, AlignTop);

        dialog_ex_set_left_button_text(app->delete_dialog, "Cancel");
        dialog_ex_set_right_button_text(app->delete_dialog, "Delete");

        view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewDeleteConfirm);
    }
}

// -------------------------------------------------------------------
// Submenu callback
// -------------------------------------------------------------------
static void theme_manager_submenu_callback(void* context, uint32_t index) {
    ThemeManagerApp* app = context;

    if(index == MENU_INDEX_RESTORE) {
        if(theme_manager_restore_backup(app)) {
            dialog_ex_set_header(
                app->reboot_dialog, "Backup Restored!", 64, 0, AlignCenter, AlignTop);
            dialog_ex_set_text(
                app->reboot_dialog,
                "Previous theme restored.\nReboot now?",
                64, 26, AlignCenter, AlignTop);
            dialog_ex_set_left_button_text(app->reboot_dialog, "Later");
            dialog_ex_set_right_button_text(app->reboot_dialog, "Reboot");
            view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewReboot);
        } else {
            theme_manager_show_error(app, "No backup found!");
        }
        return;
    }

    if(index >= app->theme_count) return;

    theme_manager_show_info(app, index);
}

// -------------------------------------------------------------------
// Confirm callback
// -------------------------------------------------------------------
static void theme_manager_confirm_callback(DialogExResult result, void* context) {
    ThemeManagerApp* app = context;

    if(result == DialogExResultRight) {
        view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewLoading);

        if(theme_manager_apply_theme(app, app->selected_index)) {
            const char* type_str = "";
            switch(app->theme_types[app->selected_index]) {
            case ThemeTypePack:
                type_str = "Pack merged";
                break;
            case ThemeTypeAnimsPack:
                type_str = "Anims merged";
                break;
            case ThemeTypeSingle:
                type_str = "Anim + manifest";
                break;
            }

            dialog_ex_set_header(
                app->reboot_dialog, "Theme Applied!", 64, 0, AlignCenter, AlignTop);

            furi_string_printf(
                app->dialog_text,
                "%s\n%s. Reboot now?",
                app->theme_names[app->selected_index],
                type_str);
            dialog_ex_set_text(
                app->reboot_dialog,
                furi_string_get_cstr(app->dialog_text),
                64, 26, AlignCenter, AlignTop);

            dialog_ex_set_left_button_text(app->reboot_dialog, "Later");
            dialog_ex_set_right_button_text(app->reboot_dialog, "Reboot");
            view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewReboot);
        } else {
            theme_manager_show_error(app, "Apply failed!\nCheck SD card.");
        }
    } else {
        theme_manager_show_info(app, app->selected_index);
    }
}

// -------------------------------------------------------------------
// Reboot callback
// -------------------------------------------------------------------
static void theme_manager_reboot_callback(DialogExResult result, void* context) {
    ThemeManagerApp* app = context;

    if(result == DialogExResultRight) {
        furi_hal_power_reset();
    } else {
        theme_manager_scan_themes(app);
        theme_manager_populate_submenu(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewSubmenu);
    }
}

// -------------------------------------------------------------------
// Delete confirmation callback
// -------------------------------------------------------------------
static void theme_manager_delete_callback(DialogExResult result, void* context) {
    ThemeManagerApp* app = context;

    if(result == DialogExResultRight) {
        if(theme_manager_delete_theme(app, app->selected_index)) {
            theme_manager_scan_themes(app);
            theme_manager_populate_submenu(app);

            popup_set_header(app->popup, "Deleted!", 64, 10, AlignCenter, AlignTop);
            popup_set_text(app->popup, "Theme removed from SD", 64, 32, AlignCenter, AlignCenter);
            popup_set_timeout(app->popup, 2000);
            popup_enable_timeout(app->popup);
            popup_set_callback(app->popup, theme_manager_popup_callback);
            popup_set_context(app->popup, app);
            view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewPopup);
        } else {
            theme_manager_show_error(app, "Delete failed!\nCheck SD card.");
        }
    } else {
        theme_manager_show_info(app, app->selected_index);
    }
}

// -------------------------------------------------------------------
// Popup timeout callback — return to submenu
// -------------------------------------------------------------------
static void theme_manager_popup_callback(void* context) {
    ThemeManagerApp* app = context;
    view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewSubmenu);
}

// -------------------------------------------------------------------
// Error popup
// -------------------------------------------------------------------
static void theme_manager_show_error(ThemeManagerApp* app, const char* message) {
    popup_set_header(app->popup, "Error", 64, 0, AlignCenter, AlignTop);
    popup_set_text(app->popup, message, 64, 32, AlignCenter, AlignCenter);
    popup_set_timeout(app->popup, 3000);
    popup_enable_timeout(app->popup);
    popup_set_callback(app->popup, theme_manager_popup_callback);
    popup_set_context(app->popup, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewPopup);
}

// -------------------------------------------------------------------
// Populate submenu with type labels
// -------------------------------------------------------------------
static void theme_manager_populate_submenu(ThemeManagerApp* app) {
    submenu_reset(app->submenu);

    if(app->theme_count == 0) {
        if(!storage_dir_exists(app->storage, ANIMATION_PACKS_PATH)) {
            submenu_add_item(app->submenu, "[No SD / No folder]", 0, NULL, NULL);
        } else {
            submenu_add_item(app->submenu, "[No themes found]", 0, NULL, NULL);
        }
    } else {
        for(uint32_t i = 0; i < app->theme_count; i++) {
            const char* prefix;
            switch(app->theme_types[i]) {
            case ThemeTypePack:
                prefix = "[P] ";
                break;
            case ThemeTypeAnimsPack:
                prefix = "[A] ";
                break;
            case ThemeTypeSingle:
                prefix = "[S] ";
                break;
            default:
                prefix = "";
                break;
            }

            snprintf(
                app->menu_labels[i],
                MAX_LABEL_LEN,
                "%s%s",
                prefix, app->theme_names[i]);

            if(strlen(app->menu_labels[i]) > 26) {
                app->menu_labels[i][23] = '.';
                app->menu_labels[i][24] = '.';
                app->menu_labels[i][25] = '.';
                app->menu_labels[i][26] = '\0';
            }

            submenu_add_item(
                app->submenu,
                app->menu_labels[i],
                i,
                theme_manager_submenu_callback,
                app);
        }
    }

    if(app->has_backup) {
        submenu_add_item(
            app->submenu, ">> Restore Previous <<", MENU_INDEX_RESTORE,
            theme_manager_submenu_callback, app);
    }
}

// -------------------------------------------------------------------
// Navigation
// -------------------------------------------------------------------
static uint32_t theme_manager_nav_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t theme_manager_nav_submenu(void* context) {
    UNUSED(context);
    return ThemeManagerViewSubmenu;
}

static uint32_t theme_manager_nav_info(void* context) {
    UNUSED(context);
    return ThemeManagerViewInfo;
}

// ===================================================================
// Entry point
// ===================================================================
int32_t theme_manager_app(void* p) {
    UNUSED(p);

    ThemeManagerApp* app = malloc(sizeof(ThemeManagerApp));
    memset(app, 0, sizeof(ThemeManagerApp));
    app->dialog_text = furi_string_alloc();

    app->storage = furi_record_open(RECORD_STORAGE);
    app->gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->submenu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->submenu), theme_manager_nav_exit);
    view_dispatcher_add_view(
        app->view_dispatcher, ThemeManagerViewSubmenu, submenu_get_view(app->submenu));

    app->info_widget = widget_alloc();
    view_set_previous_callback(widget_get_view(app->info_widget), theme_manager_nav_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, ThemeManagerViewInfo, widget_get_view(app->info_widget));

    app->confirm_dialog = dialog_ex_alloc();
    dialog_ex_set_result_callback(app->confirm_dialog, theme_manager_confirm_callback);
    dialog_ex_set_context(app->confirm_dialog, app);
    view_set_previous_callback(
        dialog_ex_get_view(app->confirm_dialog), theme_manager_nav_info);
    view_dispatcher_add_view(
        app->view_dispatcher, ThemeManagerViewConfirm,
        dialog_ex_get_view(app->confirm_dialog));

    app->reboot_dialog = dialog_ex_alloc();
    dialog_ex_set_result_callback(app->reboot_dialog, theme_manager_reboot_callback);
    dialog_ex_set_context(app->reboot_dialog, app);
    view_set_previous_callback(
        dialog_ex_get_view(app->reboot_dialog), theme_manager_nav_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, ThemeManagerViewReboot,
        dialog_ex_get_view(app->reboot_dialog));

    app->delete_dialog = dialog_ex_alloc();
    dialog_ex_set_result_callback(app->delete_dialog, theme_manager_delete_callback);
    dialog_ex_set_context(app->delete_dialog, app);
    view_set_previous_callback(
        dialog_ex_get_view(app->delete_dialog), theme_manager_nav_info);
    view_dispatcher_add_view(
        app->view_dispatcher, ThemeManagerViewDeleteConfirm,
        dialog_ex_get_view(app->delete_dialog));

    app->popup = popup_alloc();
    view_set_previous_callback(popup_get_view(app->popup), theme_manager_nav_submenu);
    view_dispatcher_add_view(
        app->view_dispatcher, ThemeManagerViewPopup, popup_get_view(app->popup));

    app->loading = loading_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ThemeManagerViewLoading, loading_get_view(app->loading));

    theme_manager_scan_themes(app);
    theme_manager_populate_submenu(app);

    view_dispatcher_switch_to_view(app->view_dispatcher, ThemeManagerViewSubmenu);
    view_dispatcher_run(app->view_dispatcher);

    view_dispatcher_remove_view(app->view_dispatcher, ThemeManagerViewLoading);
    view_dispatcher_remove_view(app->view_dispatcher, ThemeManagerViewPopup);
    view_dispatcher_remove_view(app->view_dispatcher, ThemeManagerViewDeleteConfirm);
    view_dispatcher_remove_view(app->view_dispatcher, ThemeManagerViewReboot);
    view_dispatcher_remove_view(app->view_dispatcher, ThemeManagerViewConfirm);
    view_dispatcher_remove_view(app->view_dispatcher, ThemeManagerViewInfo);
    view_dispatcher_remove_view(app->view_dispatcher, ThemeManagerViewSubmenu);

    loading_free(app->loading);
    popup_free(app->popup);
    dialog_ex_free(app->delete_dialog);
    dialog_ex_free(app->reboot_dialog);
    dialog_ex_free(app->confirm_dialog);
    widget_free(app->info_widget);
    submenu_free(app->submenu);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);

    furi_string_free(app->dialog_text);
    free(app);

    return 0;
}
