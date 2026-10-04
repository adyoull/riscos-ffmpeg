#ifndef FAKE_SDL_GL_H
#define FAKE_SDL_GL_H
extern double fake_time, fake_queued_total;
int fake_audio_bps(void);           /* the (fake) sound output's bytes a second, as opened */
extern double fake_audio_rate;          /* 1.0: the sound plays exactly at the timer's rate */
double fake_audio_seconds(void);        /* seconds of sound played so far */
extern int fake_audio_fail, fake_audio_open, fake_audio_paused, fake_audio_stall;
extern unsigned char *fake_tex;
extern int fake_tex_w, fake_tex_h, fake_tex_images, fake_tex_subimages;
extern int fake_gl_eglimage, fake_gl_context, fake_img_fail;
extern int fake_images, fake_img_binds, fake_img_w, fake_img_h, fake_img_bgr, fake_destroyed_linked;
extern unsigned int fake_tex_linked;
extern unsigned char *fake_img_pixels;
#endif
