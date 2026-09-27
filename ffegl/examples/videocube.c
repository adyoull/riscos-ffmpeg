/*
 * videocube - a video on a spinning cube: ffegl_texture() with OpenGL 1.x
 * (riscos-mesa's freeglut, EGL and OSMesa).
 *
 *   videocube [-flat] FILE
 *
 * Each new frame goes into a GL texture (through an EGLImage, with no copy,
 * on riscos-mesa 7pre12 or later); the cube is drawn every frame.
 * -flat draws the video on a flat quad instead (the plain "video in GL"
 * case). Keys: Space pause, Escape or Q quit.
 *
 * Software OpenGL draws the texture, so keep the window small (the default
 * is 480x360): the cost grows with the pixels covered.
 *
 * Part of riscos-ffmpeg: an example for ffegl. MIT licence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GL/freeglut.h>
#include "ffegl.h"

static ReelCore *video;
static GLuint tex;
static int flat, have_frame, frames_drawn;
static float angle;
static int last_ms;
static int win_w = 480, win_h = 360;

static void draw_quad(float s)
{
    /* the picture's top row is t = 0 */
    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex3f(-s, -1, 0);
    glTexCoord2f(1, 1); glVertex3f( s, -1, 0);
    glTexCoord2f(1, 0); glVertex3f( s,  1, 0);
    glTexCoord2f(0, 0); glVertex3f(-s,  1, 0);
    glEnd();
}

static void draw_cube(void)
{
    static const float rot[6][4] = {
        { 0, 0, 1, 0 }, { 90, 0, 1, 0 }, { 180, 0, 1, 0 },
        { 270, 0, 1, 0 }, { 90, 1, 0, 0 }, { -90, 1, 0, 0 } };
    for (int i = 0; i < 6; i++) {
        glPushMatrix();
        glRotatef(rot[i][0], rot[i][1], rot[i][2], rot[i][3]);
        glTranslatef(0, 0, 1);
        draw_quad(1);
        glPopMatrix();
    }
}

static void display(void)
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (have_frame) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
        glLoadIdentity();
        if (flat) {
            /* fit the picture in the window, keeping its shape */
            double va = (double)reelcore_width(video) / reelcore_height(video);
            double wa = (double)win_w / (win_h ? win_h : 1);
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            if (wa > va) glOrtho(-wa, wa, -1, 1, -1, 1);
            else         glOrtho(-va, va, -va / wa, va / wa, -1, 1);
            glMatrixMode(GL_MODELVIEW);
            draw_quad((float)va);
            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glMatrixMode(GL_MODELVIEW);
        } else {
            glTranslatef(0, 0, -5);
            glRotatef(20, 1, 0, 0);
            glRotatef(angle, 0, 1, 0);
            draw_cube();
        }
        glDisable(GL_TEXTURE_2D);
    }
    glutSwapBuffers();
    frames_drawn++;
}

static void reshape(int w, int h)
{
    win_w = w;
    win_h = h;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45, (double)w / (h ? h : 1), 0.5, 20);
    glMatrixMode(GL_MODELVIEW);
}

static void idle(void)
{
    int r = reelcore_update(video), now = glutGet(GLUT_ELAPSED_TIME);
    if (r == REELCORE_END || r < 0) {
        glutLeaveMainLoop();
        return;
    }
    if (r == REELCORE_NEW_FRAME && ffegl_texture(video, tex, &tex) == 0 && !have_frame) {
        GLint ifmt = 0;
        have_frame = 1;
        glBindTexture(GL_TEXTURE_2D, tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        /* the title says how the frames reach the texture */
        glutSetWindowTitle(ifmt == GL_RGB ? (flat ? "Video (OpenGL, EGLImage)" : "Video cube (EGLImage)")
                                          : (flat ? "Video (OpenGL, copies)" : "Video cube (copies)"));
    }
    if (!reelcore_paused(video))
        angle += (now - last_ms) * 0.03f;   /* 30 degrees a second */
    last_ms = now;
    if (!flat || r == REELCORE_NEW_FRAME)
        glutPostRedisplay();
}

static void keyboard(unsigned char k, int x, int y)
{
    if (k == 27 || k == 'q' || k == 'Q')
        glutLeaveMainLoop();
    else if (k == ' ')
        reelcore_pause(video, !reelcore_paused(video));
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    glutInit(&argc, argv);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-flat")) flat = 1;
        else file = argv[i];
    }
    if (!file) {
        fprintf(stderr, "usage: videocube [-flat] FILE\n");
        return 1;
    }
    video = reelcore_open(file, REELCORE_LOOP);
    if (!video) {
        fprintf(stderr, "videocube: %s\n", reelcore_last_error());
        return 1;
    }
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    glutInitWindowSize(480, 360);
    glutCreateWindow(flat ? "Video (OpenGL)" : "Video cube");
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.1f, 0.15f, 1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    glutKeyboardFunc(keyboard);
    last_ms = glutGet(GLUT_ELAPSED_TIME);
    glutMainLoop();
    reelcore_close(video);
    return 0;
}
