/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_draw.c -- drawing

#include "tr_local.h"
#include "tr_hzm_framegen.h" // HZM frame generation (r_hzmFrameGen)

vec4_t r_colorWhite = { 1.0, 1.0, 1.0, 1.0 };

/*
================
Draw_SetColor
================
*/
void Draw_SetColor(const vec4_t rgba) {
#if 1
    if (!rgba) {
        rgba = r_colorWhite;
    }

    backEnd.color2D[0] = (byte)(rgba[0] * tr.identityLightByte);
    backEnd.color2D[1] = (byte)(rgba[1] * tr.identityLightByte);
    backEnd.color2D[2] = (byte)(rgba[2] * tr.identityLightByte);
    backEnd.color2D[3] = (byte)(rgba[3] * 255.0);
#else
    RE_SetColor(rgba);
#endif
}

/*
================
Draw_StretchPic
================
*/
// HZM gl2 (bug #73 viewmodel-ghost-over-menus): in menu-only frames (fullscreen ESC board,
// main menu after disconnect) the client submits NO scene, but tr.renderFbo still holds the
// LAST GAMEPLAY FRAME - the stale image (weapon silhouette) showed through translucent menu
// regions as a dark "ghost gun". gl1 has no persistent offscreen target so it never haunts.
// Track whether a scene was drawn since the last swap; the FIRST immediate 2D draw of a
// sceneless frame clears the render target before painting the UI.
int g_sceneThisFrame = 0;

// bug #73 secondary fix: C shim so C++ TUs (tr_font.cpp) can bind the render FBO without
// fighting FBO_Bind's unguarded header linkage.
void R_BindRenderFbo(void)
{
    if (glRefConfig.framebufferObject) {
        FBO_Bind(tr.renderFbo);
    }
}

void R_Ensure2DClear(void)
{
    // SUPERSEDED (bug #73 final fix): RB_SwapBuffers now clears tr.renderFbo after every
    // present, which covers all frame shapes with no sceneless detection - the detection
    // here was silently defeated by UI 3D-model preview scenes (RDF_HUD) setting
    // g_sceneThisFrame. Kept as a no-op until the diagnostic scaffolding strip.
}

void Draw_StretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, qhandle_t hShader) {
#if 1
    shader_t* shader;

    R_IssuePendingRenderCommands();
    R_Ensure2DClear();

    if (glRefConfig.framebufferObject)
        FBO_Bind(tr.renderFbo);

    if (hShader) {
        shader = R_GetShaderByHandle(hShader);
    }
    else {
        shader = tr.defaultShader;
    }

    if (w <= 0) {
        w = shader->stages[0]->bundle[0].image[0]->width;
        h = shader->stages[0]->bundle[0].image[0]->height;
    }
    R_FG_Note2D(w, h, shader->name);   // HZM frame generation: a big 2D quad over the scene = an overlay is up

    // draw the pic
    RB_BeginSurface(shader, 0, 0);

    // HZM gl2 (bug-gl2-compass / bug-gl2-hudsegments): feed the 2D stream vertex color EXACTLY as
    // renderergl1 does. gl1's Draw_* helpers pass the 0..255 backEnd.color2D BYTES straight into
    // RB_Color4f(), whose `a * 255.0` (and rgb * identityLightByte) overflows the byte store into a
    // one's-complement of the input (255 -> 1). The HZM "Airborne" HUD shaders
    // (scripts/hud_airborne_coop.shader: hud_compass_*, hud_health_*) are authored with
    // `alphaGen oneMinusVertex`, which inverts a SECOND time, so the two inversions cancel and the
    // element gets its intended opacity. Using RB_Color4bv() here (no inversion) made oneMinusVertex
    // singly-invert opaque HUD elements to alpha 0 -> the compass (all parts) and the health segment
    // overlay (hud_health_out) rendered fully transparent. Default LIGHTMAP_2D pics (menus, the cyan
    // health fill) use CGEN_GLOBAL_COLOR/AGEN_GLOBAL_ALPHA and ignore vertex color, so are unaffected.
    RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

    RB_Texcoord2f(s1, t1);
    RB_Vertex2f(x, y);

    RB_Texcoord2f(s2, t1);
    RB_Vertex2f(x + w, y);

    RB_Texcoord2f(s1, t2);
    RB_Vertex2f(x, y + h);

    RB_Texcoord2f(s2, t2);
    RB_Vertex2f(x + w, y + h);

    RB_StreamEnd();
#else
    RE_StretchPic(x, y, w, h, s1, t1, s2, t2, hShader);
#endif
}

/*
================
Draw_StretchPic2
================
*/
void Draw_StretchPic2(float x, float y, float w, float h, float s1, float t1, float s2, float t2, float sx, float sy, qhandle_t hShader) {
    shader_t* shader;
    float halfWidth, halfHeight;
    float scaledWidth1, scaledHeight1;
    float scaledWidth2, scaledHeight2;

    R_IssuePendingRenderCommands();
    R_Ensure2DClear();
    R_FG_Note2D(w, h, "stretchpic2");   // HZM frame generation: a big 2D quad over the scene = an overlay is up

    if (glRefConfig.framebufferObject)
        FBO_Bind(tr.renderFbo);

    if (hShader) {
        shader = R_GetShaderByHandle(hShader);
    }
    else {
        shader = tr.defaultShader;
    }

    if (w <= 0) {
        w = shader->stages[0]->bundle[0].image[0]->width;
        h = shader->stages[0]->bundle[0].image[0]->height;
    }

    halfWidth = w * 0.5f;
    halfHeight = h * 0.5f;
    scaledWidth1 = halfWidth * sy;
    scaledHeight1 = halfHeight * sx;
    scaledWidth2 = halfWidth * sx;
    scaledHeight2 = halfHeight * sy;

    // draw the pic
    // HZM gl2 (bug-gl2-compass): match gl1 vertex-color feed so alphaGen oneMinusVertex cancels. See Draw_StretchPic.
    RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);
    RB_BeginSurface(shader, 0, 0);

    RB_Texcoord2f(s1, t1);
    RB_Vertex3f(x + halfWidth - (scaledWidth2 + -scaledHeight2), y + halfWidth - scaledHeight1 - scaledWidth1, 0);

    RB_Texcoord2f(t2, t1);
    RB_Vertex3f(scaledWidth2 - -scaledHeight2 + x + halfWidth, scaledWidth1 - scaledHeight1 + y + halfWidth, 0);

    RB_Texcoord2f(s1, s2);
    RB_Vertex3f(x+ halfWidth - (scaledWidth2 + scaledHeight2), scaledHeight1 - scaledWidth1 + y + halfWidth, 0);

    RB_Texcoord2f(t2, s2);
    RB_Vertex3f(scaledWidth2 - scaledHeight2 + x + halfWidth, scaledWidth1 + scaledHeight1 + y + halfWidth, 0);

    RB_StreamEnd();
}


/*
================
Draw_TilePic
================
*/
void Draw_TilePic(float x, float y, float w, float h, qhandle_t hShader) {
    R_Ensure2DClear();
    shader_t* shader;
    float        picw, pich;

    R_IssuePendingRenderCommands();
    R_FG_Note2D(w, h, "tilepic");   // HZM frame generation: a big 2D quad over the scene = an overlay is up

    if (glRefConfig.framebufferObject)
        FBO_Bind(tr.renderFbo);

    if (hShader) {
        shader = R_GetShaderByHandle(hShader);
    }
    else {
        shader = tr.defaultShader;
    }

    if (w <= 0) {
        w = shader->stages[0]->bundle[0].image[0]->width;
        h = shader->stages[0]->bundle[0].image[0]->height;
    }

    picw = shader->stages[0]->bundle[0].image[0]->uploadWidth;
    pich = shader->stages[0]->bundle[0].image[0]->uploadHeight;

    // draw the pic
    // HZM gl2 (bug-gl2-hudsegments): match gl1 vertex-color feed so alphaGen oneMinusVertex cancels. See Draw_StretchPic.
    RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

    RB_StreamBegin(shader);

    RB_Texcoord2f(x / picw, y / pich);
    RB_Vertex2f(x, y);

    RB_Texcoord2f((x + w) / picw, y / pich);
    RB_Vertex2f(x + w, y);

    RB_Texcoord2f(x / picw, (y + h) / pich);
    RB_Vertex2f(x, y + h);

    RB_Texcoord2f((x + w) / picw, (y + h) / pich);
    RB_Vertex2f(x + w, y + h);

    RB_StreamEnd();
}

/*
================
Draw_TilePicOffset
================
*/
void Draw_TilePicOffset(float x, float y, float w, float h, qhandle_t hShader, int offsetX, int offsetY) {
    R_Ensure2DClear();
    shader_t* shader;
    float        picw, pich;

    R_IssuePendingRenderCommands();
    R_FG_Note2D(w, h, "tilepic");   // HZM frame generation: a big 2D quad over the scene = an overlay is up

    if (glRefConfig.framebufferObject)
        FBO_Bind(tr.renderFbo);

    if (hShader) {
        shader = R_GetShaderByHandle(hShader);
    }
    else {
        shader = tr.defaultShader;
    }

    if (w <= 0) {
        w = shader->stages[0]->bundle[0].image[0]->width;
        h = shader->stages[0]->bundle[0].image[0]->height;
    }

    picw = shader->stages[0]->bundle[0].image[0]->uploadWidth;
    pich = shader->stages[0]->bundle[0].image[0]->uploadHeight;

    // draw the pic
    // HZM gl2 (bug-gl2-hudsegments): match gl1 vertex-color feed so alphaGen oneMinusVertex cancels. See Draw_StretchPic.
    RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

    RB_StreamBegin(shader);

    RB_Texcoord2f(x / picw, y / pich);
    RB_Vertex2f(x + offsetX, y + offsetY);

    RB_Texcoord2f((x + w) / picw, y / pich);
    RB_Vertex2f(x + offsetX + w, y + offsetY);

    RB_Texcoord2f(x / picw, (y + h) / pich);
    RB_Vertex2f(x + offsetX, y + offsetY + h);

    RB_Texcoord2f((x + w) / picw, (y + h) / pich);
    RB_Vertex2f(x + offsetX + w, y + offsetY + h);

    RB_StreamEnd();
}

/*
================
Draw_TrianglePic
================
*/
void Draw_TrianglePic(const vec2_t vPoints[3], const vec2_t vTexCoords[3], qhandle_t hShader) {
    R_Ensure2DClear();
    int            i;
    shader_t* shader;

    R_IssuePendingRenderCommands();

    if (glRefConfig.framebufferObject)
        FBO_Bind(tr.renderFbo);

    if (hShader) {
        shader = R_GetShaderByHandle(hShader);
    }
    else {
        shader = tr.defaultShader;
    }

    // draw the pic
    // HZM gl2 (bug-gl2-compass): the compass rose (cl_uistd.cpp DrawStatCompass) draws hud_compass_in,
    // an alphaGen oneMinusVertex shader. Match gl1's inverted vertex-color feed so it cancels. See Draw_StretchPic.
    RB_Color4f(backEnd.color2D[0], backEnd.color2D[1], backEnd.color2D[2], backEnd.color2D[3]);

    RB_BeginSurface(shader, 0, 0);

    for (i = 0; i < 3; i++) {
        RB_Texcoord2f(vTexCoords[i][0], vTexCoords[i][1]);
        RB_Vertex2f(vPoints[i][0], vPoints[i][1]);
    }

    RB_StreamEnd();
}

/*
================
RE_DrawBackground_TexSubImage
================
*/
void RE_DrawBackground_TexSubImage(int cols, int rows, int bgr, byte* data) {
    // FIXME: unimplemented (GL2)
#if 0
    GLenum    format;
    int        w, h;

    w = glConfig.vidWidth;
    h = glConfig.vidHeight;

    R_IssuePendingRenderCommands();
    qglFinish();

    if (bgr) {
        format = GL_BGR_EXT;
    }
    else {
        format = GL_RGB;
    }

    GL_Bind(tr.scratchImage);

    if (cols == tr.scratchImage->width && rows == tr.scratchImage->height && format == tr.scratchImage->internalFormat)
    {
        qglTexSubImage2D(3553, 0, 0, 0, cols, rows, format, 5121, data);
    }
    else
    {
        tr.scratchImage->uploadWidth = cols;
        tr.scratchImage->uploadHeight = rows;
        tr.scratchImage->internalFormat = format;
        qglTexImage2D(GL_TEXTURE_2D, 0, 3, cols, rows, 0, format, 5121, data);
        qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, 9729.0);
        qglTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, 9729.0);
    }

    qglDisable(GL_CULL_FACE);
    qglDisable(GL_DEPTH_TEST);
    qglEnable(GL_TEXTURE_2D);

    qglBegin(GL_QUADS);

    qglTexCoord2f(0.5 / (GLfloat)cols, ((GLfloat)rows - 0.5) / rows);
    qglVertex2f(0, 0);

    qglTexCoord2f(((GLfloat)cols - 0.5) / cols, ((GLfloat)rows - 0.5) / rows);
    qglVertex2f(w, 0);

    qglTexCoord2f(((GLfloat)cols - 0.5) / cols, 0.5 / (GLfloat)rows);
    qglVertex2f(w, h);

    qglTexCoord2f(0.5 / (GLfloat)rows, 0.5 / (GLfloat)rows);
    qglVertex2f(0, h);

    qglEnd();
#endif
}

/*
================
RE_DrawBackground_DrawPixels
================
*/
void RE_DrawBackground_DrawPixels(int cols, int rows, int bgr, byte* data) {
    // FIXME: unimplemented (GL2)
#if 0
    R_IssuePendingRenderCommands();

    GL_State(0);
    qglDisable(GL_TEXTURE_2D);

    qglPixelZoom(glConfig.vidWidth / rows, glConfig.vidHeight / cols);

    if (bgr) {
        qglDrawPixels(cols, rows, GL_BGR, GL_UNSIGNED_BYTE, data);
    } else {
        qglDrawPixels(cols, rows, GL_RGB, GL_UNSIGNED_BYTE, data);
    }

    qglPixelZoom(1.0, 1.0);

    qglEnable(GL_TEXTURE_2D);
#endif
}

/*
================
AddBox
================
*/
void AddBox(float x, float y, float w, float h) {
    R_Ensure2DClear();
    vec4_t quadVerts[4];
    vec2_t texCoords[4];
    vec4_t color;

    R_IssuePendingRenderCommands();
    R_FG_Note2D(w, h, "(additive box)");   // HZM frame generation: a big 2D quad over the scene = an overlay is up

    GL_BindToTMU(tr.whiteImage, TB_COLORMAP);
    GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

    if (glRefConfig.framebufferObject)
    {
        FBO_Bind(tr.renderFbo);
    }

    color[0] = backEnd.color2D[0] / 255.0;
    color[1] = backEnd.color2D[1] / 255.0;
    color[2] = backEnd.color2D[2] / 255.0;
    color[3] = backEnd.color2D[3] / 255.0;
    
    VectorSet4(quadVerts[0], x,     y,     0.0f, 1.0f);
    VectorSet4(quadVerts[1], x + w, y,     0.0f, 1.0f);
    VectorSet4(quadVerts[2], x + w, y + h, 0.0f, 1.0f);
    VectorSet4(quadVerts[3], x,     y + h, 0.0f, 1.0f);

    VectorSet2(texCoords[0], 0.0f, 0.0f);
    VectorSet2(texCoords[1], 1.0f, 0.0f);
    VectorSet2(texCoords[2], 1.0f, 1.0f);
    VectorSet2(texCoords[3], 0.0f, 1.0f);

    GLSL_BindProgram(&tr.textureColorShader);
    
    GLSL_SetUniformMat4(&tr.textureColorShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
    GLSL_SetUniformVec4(&tr.textureColorShader, UNIFORM_COLOR, color);

    RB_InstantQuad2(quadVerts, texCoords);
}

/*
================
DrawBox
================
*/
void DrawBox(float x, float y, float w, float h) {
    R_Ensure2DClear();
    // HZM gl2 re-port (bug-gl2-introfade): the scripted fadein/fadeout screen fade
    // (View3D::DrawFades -> re.DrawBox, driven by ps.blend) and other solid-fill 2D
    // boxes were not visible under gl2 during the mission-intro card, while every
    // stream-pipeline 2D element (Draw_StretchPic text/pics) in the very same frames
    // rendered fine. The old implementation drew an "instant quad" through
    // tr.textureColorShader and depended on ambient front-end state caches
    // (glState.modelviewProjection / GL_State bits / GL_Cull / TMU cache) being valid
    // at call time. Route the box through the exact same immediate stream/material
    // pipeline Draw_StretchPic uses instead: a default 2D (LIGHTMAP_2D) shader over
    // tr.whiteImage, which after Fix 2 carries CGEN_GLOBAL_COLOR / AGEN_GLOBAL_ALPHA
    // (= backEnd.color2D, identical color semantics to the old path) and
    // GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA
    // (identical blend). This is the proven-working recipe; no state-cache dependency.
    shader_t *shader;

    R_IssuePendingRenderCommands();
    R_FG_Note2D(w, h, "(box)");   // HZM frame generation: a big 2D quad over the scene = an overlay is up

    if (glRefConfig.framebufferObject)
    {
        FBO_Bind(tr.renderFbo);
    }

    // "*white" resolves via the image hash (tr.whiteImage) - no disk access; the
    // per-call lookup keeps the handle valid across level loads / shader resets.
    shader = R_FindShader("*white", LIGHTMAP_2D, qfalse);

    RB_BeginSurface(shader, 0, 0);

    RB_Color4bv(backEnd.color2D);

    RB_Texcoord2f(0.0f, 0.0f);
    RB_Vertex2f(x, y);

    RB_Texcoord2f(1.0f, 0.0f);
    RB_Vertex2f(x + w, y);

    RB_Texcoord2f(0.0f, 1.0f);
    RB_Vertex2f(x, y + h);

    RB_Texcoord2f(1.0f, 1.0f);
    RB_Vertex2f(x + w, y + h);

    RB_StreamEnd();
}

/*
================
DrawLineLoop
================
*/
void DrawLineLoop(const vec2_t* points, int count, int stipple_factor, int stipple_mask) {
    // HZM gl2 re-port: was a FIXME stub - UI/HUD outline rectangles (borders) drawn via
    // line loops were simply invisible under gl2. Core-profile-safe implementation: draw
    // each segment as a 1px-wide quad through the same textureColorShader path as DrawBox.
    // Stipple patterns render solid (acceptable vs not rendering at all).
    int i;
    vec4_t color;

    if (count < 2) {
        return;
    }

    R_IssuePendingRenderCommands();

    GL_BindToTMU(tr.whiteImage, TB_COLORMAP);
    GL_State(GLS_DEPTHTEST_DISABLE | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_SRCBLEND_SRC_ALPHA);

    if (glRefConfig.framebufferObject)
    {
        FBO_Bind(tr.renderFbo);
    }

    color[0] = backEnd.color2D[0] / 255.0;
    color[1] = backEnd.color2D[1] / 255.0;
    color[2] = backEnd.color2D[2] / 255.0;
    color[3] = backEnd.color2D[3] / 255.0;

    GLSL_BindProgram(&tr.textureColorShader);
    GLSL_SetUniformMat4(&tr.textureColorShader, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
    GLSL_SetUniformVec4(&tr.textureColorShader, UNIFORM_COLOR, color);

    for (i = 0; i < count; i++) {
        const float *a = points[i];
        const float *b = points[(i + 1) % count];
        float dx = b[0] - a[0];
        float dy = b[1] - a[1];
        float len = (float)sqrt(dx * dx + dy * dy);
        float nx, ny;
        vec4_t quadVerts[4];
        vec2_t texCoords[4];

        if (len < 0.001f) {
            continue;
        }
        // unit normal, half-pixel each side => 1px line
        nx = -(dy / len) * 0.5f;
        ny =  (dx / len) * 0.5f;

        VectorSet4(quadVerts[0], a[0] + nx, a[1] + ny, 0.0f, 1.0f);
        VectorSet4(quadVerts[1], b[0] + nx, b[1] + ny, 0.0f, 1.0f);
        VectorSet4(quadVerts[2], b[0] - nx, b[1] - ny, 0.0f, 1.0f);
        VectorSet4(quadVerts[3], a[0] - nx, a[1] - ny, 0.0f, 1.0f);

        VectorSet2(texCoords[0], 0.0f, 0.0f);
        VectorSet2(texCoords[1], 1.0f, 0.0f);
        VectorSet2(texCoords[2], 1.0f, 1.0f);
        VectorSet2(texCoords[3], 0.0f, 1.0f);

        RB_InstantQuad2(quadVerts, texCoords);
    }

    (void)stipple_factor; (void)stipple_mask;
#if 0
    int        i;

    R_IssuePendingRenderCommands();

    qglDisable(GL_TEXTURE_2D);

    if (stipple_factor) {
        qglEnable(GL_LINE_STIPPLE);
        qglLineStipple(stipple_factor, stipple_mask);
    }

    qglBegin(GL_LINE_LOOP);

    for (i = 0; i < count; i++) {
        qglVertex2f(points[i][0], points[i][1]);
    }

    qglEnd();

    qglEnable(GL_TEXTURE_2D);

    if (stipple_factor) {
        qglDisable(GL_LINE_STIPPLE);
    }
#endif
}

/*
================
Set2DWindow
================
*/
void Set2DWindow(int x, int y, int w, int h, float left, float right, float bottom, float top, float n, float f) {
    mat4_t matrix;
    // HZM gl2 (bug-1147): remember whether we were ALREADY in 2D before the flag is raised below.
    // The stock gl2 port set backEnd.projection2D = qtrue here and then tested
    // `if (!backEnd.projection2D)` at the bottom - a dead branch, so backEnd.refdef.time /
    // floatTime were never refreshed on this path. renderergl1 gets it right because it only sets
    // backEnd.in2D INSIDE that guard. The immediate 2D draws the UI uses (Draw_StretchPic etc.)
    // reach RB_BeginSurface without going through RB_SetGL2D, so tess.shaderTime kept whatever
    // floatTime the last 3D scene left behind => animated menu/HUD shaders were frozen in gl2.
    qboolean wasIn2D;

    R_IssuePendingRenderCommands();
    R_FG_Set2DWindow(right - left, bottom - top, w, h);   // HZM frame generation: the size 2D quads are measured against

    wasIn2D = backEnd.projection2D;
    backEnd.projection2D = qtrue;
    backEnd.last2DFBO = glState.currentFBO;

    qglViewport(x, y, w, h);
    qglScissor(x, y, w, h);

    Mat4Ortho(left, right, bottom, top, n, f, matrix);
    GL_SetProjectionMatrix(matrix);
    Mat4Identity(matrix);
    GL_SetModelviewMatrix(matrix);

    GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);

    GL_Cull(CT_TWO_SIDED);

    if (!wasIn2D)
    {
        backEnd.refdef.time = ri.Milliseconds();
        backEnd.refdef.floatTime = backEnd.refdef.time / 1000.0;
    }
}

/*
================
RE_Scissor
================
*/
void RE_Scissor(int x, int y, int width, int height) {
    qglEnable(GL_SCISSOR_TEST);
    qglScissor(x, y, width, height);
}
