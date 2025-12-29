// 文件：gui_adapter.h
#ifndef GUI_ADAPTER_H
#define GUI_ADAPTER_H

#include "frontend_sdk.h"

// 支持的GUI框架
typedef enum {
    GUI_FRAMEWORK_QT = 0,
    GUI_FRAMEWORK_GTK = 1,
    GUI_FRAMEWORK_WIN32 = 2,
    GUI_FRAMEWORK_COCOA = 3,
    GUI_FRAMEWORK_WEB = 4,
    GUI_FRAMEWORK_CUSTOM = 255
} gui_framework_t;

// 窗口类型
typedef enum {
    WINDOW_MAIN_CHAT = 0,
    WINDOW_LOGIN = 1,
    WINDOW_SETTINGS = 2,
    WINDOW_FILE_TRANSFER = 3,
    WINDOW_USER_SEARCH = 4,
    WINDOW_GROUP_MANAGEMENT = 5
} window_type_t;

// 控件类型
typedef enum {
    CONTROL_BUTTON = 0,
    CONTROL_TEXT_INPUT = 1,
    CONTROL_LIST_VIEW = 2,
    CONTROL_TREE_VIEW = 3,
    CONTROL_TAB_VIEW = 4,
    CONTROL_PROGRESS_BAR = 5,
    CONTROL_LABEL = 6,
    CONTROL_IMAGE = 7
} control_type_t;

// GUI事件
typedef struct {
    window_type_t window_type;
    uint32_t control_id;
    const char *event_name;
    const void *event_data;
    size_t data_size;
} gui_event_t;

// 控件描述
typedef struct {
    uint32_t id;
    control_type_t type;
    char name[64];
    int x, y, width, height;
    void *properties;
    void (*event_handler)(gui_event_t *event, void *user_data);
} control_descriptor_t;

// 窗口描述
typedef struct {
    window_type_t type;
    char title[128];
    int width, height;
    control_descriptor_t *controls;
    size_t control_count;
    void *window_handle;
} window_descriptor_t;

// GUI适配器主结构
typedef struct {
    gui_framework_t framework;
    frontend_sdk_t *sdk;
    
    // 窗口管理
    window_descriptor_t *windows;
    size_t window_count;
    
    // 事件循环
    struct {
        bool running;
        pthread_t event_thread;
        void (*process_events)(void);
    } event_loop;
    
    // 渲染引擎
    struct {
        void (*render_window)(window_descriptor_t *window);
        void (*update_control)(uint32_t window_id, uint32_t control_id, void *data);
    } renderer;
    
    // 主题管理
    struct {
        char theme_name[64];
        void *theme_data;
        size_t theme_size;
    } theme;
} gui_adapter_t;

// ==================== GUI适配器API ====================
gui_adapter_t* gui_adapter_create(gui_framework_t framework);
void gui_adapter_destroy(gui_adapter_t *adapter);

// 初始化
int gui_adapter_init(gui_adapter_t *adapter, frontend_sdk_t *sdk);
int gui_adapter_run(gui_adapter_t *adapter);
int gui_adapter_stop(gui_adapter_t *adapter);

// 窗口管理
int gui_adapter_create_window(gui_adapter_t *adapter, window_type_t type,
                             const char *title, int width, int height,
                             uint32_t *window_id);
int gui_adapter_destroy_window(gui_adapter_t *adapter, uint32_t window_id);
int gui_adapter_show_window(gui_adapter_t *adapter, uint32_t window_id);
int gui_adapter_hide_window(gui_adapter_t *adapter, uint32_t window_id);

// 控件管理
int gui_adapter_add_control(gui_adapter_t *adapter, uint32_t window_id,
                           const control_descriptor_t *control);
int gui_adapter_remove_control(gui_adapter_t *adapter, uint32_t window_id,
                              uint32_t control_id);
int gui_adapter_update_control(gui_adapter_t *adapter, uint32_t window_id,
                              uint32_t control_id, void *data);

// 消息框和对话框
int gui_adapter_show_message(gui_adapter_t *adapter, const char *title,
                            const char *message, uint32_t message_type);
int gui_adapter_show_dialog(gui_adapter_t *adapter, const char *title,
                           const char *message, const char **buttons,
                           size_t button_count, int *selected);

// 文件对话框
int gui_adapter_show_open_dialog(gui_adapter_t *adapter, const char *title,
                                const char *filters, char **selected_path);
int gui_adapter_show_save_dialog(gui_adapter_t *adapter, const char *title,
                                const char *default_name, char **selected_path);
int gui_adapter_show_directory_dialog(gui_adapter_t *adapter, const char *title,
                                     char **selected_path);

// 主题管理
int gui_adapter_set_theme(gui_adapter_t *adapter, const char *theme_name,
                         const void *theme_data, size_t theme_size);
int gui_adapter_load_theme(gui_adapter_t *adapter, const char *theme_file);

// 事件处理
int gui_adapter_send_event(gui_adapter_t *adapter, const gui_event_t *event);
int gui_adapter_process_pending_events(gui_adapter_t *adapter);

// 工具函数
const char* gui_framework_to_string(gui_framework_t framework);
bool gui_adapter_is_supported(gui_framework_t framework);
int gui_adapter_get_screen_size(gui_adapter_t *adapter, int *width, int *height);

#endif // GUI_ADAPTER_H