#ifndef MENU_H
#define MENU_H

/* 菜单状态 */
extern int menu_active;      /* 1 = 菜单正在显示 */

/* 初始化菜单系统 */
void menu_init(void);

/* 进入/退出菜单模式 */
void menu_enter(void);
void menu_exit(void);

/* 菜单输入处理（由 input.c 调用） */
int menu_handle_input(int key, int is_down);

/* 每帧绘制（由 VGA 循环调用） */
void menu_tick(void);

/* 通知菜单需要更新 */
void menu_mark_dirty(void);

/* HUD 信息显示 */
extern int display_info_on;
void hud_toggle(void);

#endif /* MENU_H */
