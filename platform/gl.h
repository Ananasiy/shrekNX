// Единая точка подключения OpenGL: mac — системный GL 4.1 core, Switch — mesa через glad.
// Общий диалект шейдеров: GLSL `#version 410 core`.
#pragma once

#if defined(__SWITCH__)
#include <glad/glad.h>
#define SNX_GL_NEEDS_LOADER 1
#elif defined(__APPLE__)
#include <OpenGL/gl3.h>
#define SNX_GL_NEEDS_LOADER 0
#else
#error "unsupported platform"
#endif

#define SNX_GL_MAJOR 4
#define SNX_GL_MINOR 1
#define SNX_GLSL_HEADER "#version 410 core\n"
