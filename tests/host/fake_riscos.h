#ifndef FAKE_RISCOS_H
#define FAKE_RISCOS_H
#define FAKE_SW 1280
#define FAKE_SH 720
extern int fake_desktop, fake_taskwindow, fake_visual, fake_close_after, fake_escape_after;
extern int fake_swaps, fake_locked, fake_tasks_open, fake_windows_open, fake_force_redraws;
extern int fake_swap_interval, fake_render_buffer, fake_win_w, fake_win_h;
extern unsigned char *fake_shown;
extern int fake_surf_w, fake_surf_h, fake_surf_pitch;
extern int fake_scr_w, fake_scr_h, fake_wa[4], fake_plots, fake_surfaces;
#endif
