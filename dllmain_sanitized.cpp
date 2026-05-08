#include "pch.h"
#include <windows.h>
#include <F:\DLL\packages\glew\build\native\include\GL/glew.h> 
#include <vector>
#include "F:\DLL\glm/glm.hpp"
#include "F:\DLL\glm/gtc/matrix_transform.hpp"
#include "F:\DLL\glm/gtc/type_ptr.hpp"
#include <mutex>
#include <unordered_map>
#include <F:\DLL\stb_image\stb_easy_font.h>
#include "PageGuard64.h"

// SANITIZED VERSION - NO AIMBOT, NO HWID
// Uses VEH hooks (can work standalone or with NPT)

std::mutex g_MatrixMutex;
std::vector<glm::mat4> g_GlobalModelMatrices;
HGLRC g_CurrentRenderingContext = nullptr;

struct ContextData {
    GLuint ssbo = 0;
    std::vector<glm::mat4> matrices;
    size_t max_matrices = 100;
    bool ssbo_initialized = false;
    glm::mat4 viewMatrix;
    glm::mat4 projectionMatrix;
    bool matrices_valid = false;
};
std::unordered_map<HGLRC, ContextData> g_ContextDataMap;

typedef BOOL(APIENTRY* twglSwapBuffers)(HDC hDc);
typedef PROC(APIENTRY* tglGetProcAddress)(LPCSTR lpszProc);
typedef void(APIENTRY* tglUniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
typedef void(APIENTRY* tglDrawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices);
typedef void(APIENTRY* tglUniform3fv)(GLint location, GLsizei count, const GLfloat* value);
typedef void(APIENTRY* tglShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);

void* g_Original_wglSwapBuffers = nullptr;
void* g_Original_wglGetProcAddress = nullptr;
void(APIENTRY* g_glUniform3fv)(GLint location, GLsizei count, const GLfloat* value) = nullptr;
void(APIENTRY* g_glUniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) = nullptr;
void(APIENTRY* g_glShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length) = nullptr;
void(APIENTRY* g_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices) = nullptr;
void(APIENTRY* g_glGetQueryObjectuiv)(GLuint id, GLenum pname, GLuint* params) = nullptr;
void(APIENTRY* g_glGetQueryObjectiv)(GLuint id, GLenum pname, GLint* params) = nullptr;

GLuint ModelMatrixId;
std::vector<glm::mat4> SaveModelMatrix;
GLuint ProjectionMatrixId;
glm::mat4 SaveProjectionMatrix;
GLuint ViewMatrixId;
glm::mat4 SaveViewMatrix;
static bool g_GlewInitialized = false;
GLint ProgramPlayers;
int SCREEN_X = 1920;
int SCREEN_Y = 1080;
std::vector<glm::vec2> CurrentFramePoints;
std::vector<glm::vec2> NextFramePoints;
GLuint g_ShaderProgram = 0;
GLuint g_GreenShaderProgram = 0;
GLuint g_RedShaderProgram = 0;
GLuint g_YellowShaderProgram = 0;
GLuint g_VertexShader = 0;
GLuint g_FragmentShader = 0;
GLuint g_GreenFragmentShader = 0;
GLuint g_RedFragmentShader = 0;
GLuint g_YellowFragmentShader = 0;
GLuint g_VAO = 0;
GLuint g_VBO = 0;

const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
uniform mat4 projection;
void main()
{
    gl_Position = projection * vec4(aPos.x, aPos.y, 0.0, 1.0);
}
)";

const char* fragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
void main()
{
    FragColor = vec4(1.0, 0.0, 0.0, 0.8);
}
)";

const char* greenFragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
void main()
{
    FragColor = vec4(0.0, 1.0, 0.0, 0.8);
}
)";

const char* redFragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
void main()
{
    FragColor = vec4(1.0, 0.0, 0.0, 0.8);
}
)";

const char* yellowFragmentShaderSource = R"(
#version 330 core
out vec4 FragColor;
void main()
{
    FragColor = vec4(1.0, 1.0, 0.0, 0.8);
}
)";

glm::vec2 clipToScreen(const glm::vec4& clipCoords, int screenWidth, int screenHeight) {
    glm::vec3 ndc;
    ndc.x = clipCoords.x / clipCoords.w;
    ndc.y = clipCoords.y / clipCoords.w;
    ndc.z = clipCoords.z / clipCoords.w;
    float screenX = (ndc.x + 1.0f) * 0.5f * screenWidth;
    float screenY = (1.0f - ndc.y) * 0.5f * screenHeight;
    return glm::vec2(screenX, screenY);
}

GLuint compileShader(const char* source, GLenum shaderType) {
    GLuint shader = glCreateShader(shaderType);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, NULL, infoLog);
    }
    return shader;
}

void initShaders() {
    g_VertexShader = compileShader(vertexShaderSource, GL_VERTEX_SHADER);
    g_FragmentShader = compileShader(fragmentShaderSource, GL_FRAGMENT_SHADER);
    g_GreenFragmentShader = compileShader(greenFragmentShaderSource, GL_FRAGMENT_SHADER);
    g_RedFragmentShader = compileShader(redFragmentShaderSource, GL_FRAGMENT_SHADER);
    g_YellowFragmentShader = compileShader(yellowFragmentShaderSource, GL_FRAGMENT_SHADER);

    g_ShaderProgram = glCreateProgram();
    glAttachShader(g_ShaderProgram, g_VertexShader);
    glAttachShader(g_ShaderProgram, g_FragmentShader);
    glLinkProgram(g_ShaderProgram);

    g_GreenShaderProgram = glCreateProgram();
    glAttachShader(g_GreenShaderProgram, g_VertexShader);
    glAttachShader(g_GreenShaderProgram, g_GreenFragmentShader);
    glLinkProgram(g_GreenShaderProgram);

    g_RedShaderProgram = glCreateProgram();
    glAttachShader(g_RedShaderProgram, g_VertexShader);
    glAttachShader(g_RedShaderProgram, g_RedFragmentShader);
    glLinkProgram(g_RedShaderProgram);

    g_YellowShaderProgram = glCreateProgram();
    glAttachShader(g_YellowShaderProgram, g_VertexShader);
    glAttachShader(g_YellowShaderProgram, g_YellowFragmentShader);
    glLinkProgram(g_YellowShaderProgram);

    glDeleteShader(g_VertexShader);
    glDeleteShader(g_FragmentShader);
    glDeleteShader(g_GreenFragmentShader);
    glDeleteShader(g_RedFragmentShader);
    glDeleteShader(g_YellowFragmentShader);

    glGenVertexArrays(1, &g_VAO);
    glGenBuffers(1, &g_VBO);
    glBindVertexArray(g_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_VBO);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

std::vector<glm::mat4> FilterMatricesByDistance(const std::vector<glm::mat4>& matrices, const glm::mat4& viewMatrix, float minDistance = 2.0f) {
    std::vector<glm::mat4> filteredMatrices;
    glm::mat4 invViewMatrix = glm::inverse(viewMatrix);
    glm::vec3 cameraPosWorld = glm::vec3(invViewMatrix[3]);
    for (const auto& modelMatrix : matrices) {
        glm::vec3 modelPosWorld = glm::vec3(modelMatrix[3]);
        float distance = glm::length(modelPosWorld - cameraPosWorld);
        if (distance >= minDistance) {
            filteredMatrices.push_back(modelMatrix);
        }
    }
    return filteredMatrices;
}

void draw3DCube(const glm::mat4& modelMatrix, const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, float lineWidth) {
    glm::vec4 point1Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(0.45f, -0.0f, 0.45f, 1.0f);
    glm::vec4 point2Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(-0.45f, -0.0f, 0.45f, 1.0f);
    glm::vec4 point3Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(0.45f, -0.0f, -0.45f, 1.0f);
    glm::vec4 point4Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(-0.45f, -0.0f, -0.45f, 1.0f);
    glm::vec4 point5Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(0.45f, 2.0f, 0.45f, 1.0f);
    glm::vec4 point6Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(-0.45f, 2.0f, 0.45f, 1.0f);
    glm::vec4 point7Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(0.45f, 2.0f, -0.45f, 1.0f);
    glm::vec4 point8Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(-0.45f, 2.0f, -0.45f, 1.0f);

    glm::vec2 point1Screen = clipToScreen(point1Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point2Screen = clipToScreen(point2Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point3Screen = clipToScreen(point3Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point4Screen = clipToScreen(point4Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point5Screen = clipToScreen(point5Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point6Screen = clipToScreen(point6Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point7Screen = clipToScreen(point7Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point8Screen = clipToScreen(point8Clip, SCREEN_X, SCREEN_Y);

    glUseProgram(g_RedShaderProgram);
    glm::mat4 orthoMatrix = glm::ortho(0.0f, (float)SCREEN_X, (float)SCREEN_Y, 0.0f, -1.0f, 1.0f);
    GLint projLoc = glGetUniformLocation(g_RedShaderProgram, "projection");
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(orthoMatrix));

    std::vector<glm::vec2> cubeVertices = {
        point1Screen, point2Screen,
        point2Screen, point4Screen,
        point4Screen, point3Screen,
        point3Screen, point1Screen,
        point5Screen, point6Screen,
        point6Screen, point8Screen,
        point8Screen, point7Screen,
        point7Screen, point5Screen,
        point1Screen, point5Screen,
        point2Screen, point6Screen,
        point3Screen, point7Screen,
        point4Screen, point8Screen
    };

    glBindVertexArray(g_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_VBO);
    glBufferData(GL_ARRAY_BUFFER, cubeVertices.size() * sizeof(glm::vec2), cubeVertices.data(), GL_DYNAMIC_DRAW);
    glLineWidth(lineWidth);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glDrawArrays(GL_LINES, 0, cubeVertices.size());
    glDisable(GL_LINE_SMOOTH);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

void InitializeSSBOForContext(HGLRC hglrc) {
    if (g_ContextDataMap[hglrc].ssbo_initialized) return;
    ContextData& ctx_data = g_ContextDataMap[hglrc];
    glGenBuffers(1, &ctx_data.ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx_data.ssbo);
    size_t size_in_bytes = ctx_data.max_matrices * sizeof(glm::mat4) + 2 * sizeof(glm::mat4);
    glBufferData(GL_SHADER_STORAGE_BUFFER, size_in_bytes, NULL, GL_DYNAMIC_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx_data.ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    ctx_data.matrices.resize(ctx_data.max_matrices);
    ctx_data.ssbo_initialized = true;
}

std::vector<glm::mat4> g_GlobalModelMatrices3918;
static int g_FrameCounter = 0;
bool aa = false;

void draw3DCube3918(const glm::mat4& modelMatrix, const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix, float lineWidth) {
    glm::vec4 point1Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(0.2f, -0.0f, 0.2f, 1.0f);
    glm::vec4 point2Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(-0.2f, -0.0f, 0.2f, 1.0f);
    glm::vec4 point3Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(0.2f, -0.0f, -0.2f, 1.0f);
    glm::vec4 point4Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(-0.2f, -0.0f, -0.2f, 1.0f);
    glm::vec4 point5Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(0.2f, 0.25f, 0.2f, 1.0f);
    glm::vec4 point6Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(-0.2f, 0.25f, 0.2f, 1.0f);
    glm::vec4 point7Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(0.2f, 0.25f, -0.2f, 1.0f);
    glm::vec4 point8Clip = projectionMatrix * viewMatrix * modelMatrix * glm::vec4(-0.2f, 0.25f, -0.2f, 1.0f);

    glm::vec2 point1Screen = clipToScreen(point1Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point2Screen = clipToScreen(point2Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point3Screen = clipToScreen(point3Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point4Screen = clipToScreen(point4Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point5Screen = clipToScreen(point5Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point6Screen = clipToScreen(point6Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point7Screen = clipToScreen(point7Clip, SCREEN_X, SCREEN_Y);
    glm::vec2 point8Screen = clipToScreen(point8Clip, SCREEN_X, SCREEN_Y);

    glUseProgram(g_YellowShaderProgram);
    glm::mat4 orthoMatrix = glm::ortho(0.0f, (float)SCREEN_X, (float)SCREEN_Y, 0.0f, -1.0f, 1.0f);
    GLint projLoc = glGetUniformLocation(g_YellowShaderProgram, "projection");
    glUniformMatrix4fv(projLoc, 1, GL_FALSE, glm::value_ptr(orthoMatrix));

    std::vector<glm::vec2> cubeVertices = {
        point1Screen, point2Screen,
        point2Screen, point4Screen,
        point4Screen, point3Screen,
        point3Screen, point1Screen,
        point5Screen, point6Screen,
        point6Screen, point8Screen,
        point8Screen, point7Screen,
        point7Screen, point5Screen,
        point1Screen, point5Screen,
        point2Screen, point6Screen,
        point3Screen, point7Screen,
        point4Screen, point8Screen
    };

    glBindVertexArray(g_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_VBO);
    glBufferData(GL_ARRAY_BUFFER, cubeVertices.size() * sizeof(glm::vec2), cubeVertices.data(), GL_DYNAMIC_DRAW);
    glLineWidth(lineWidth);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    glDrawArrays(GL_LINES, 0, cubeVertices.size());
    glDisable(GL_LINE_SMOOTH);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

// INLINE HOOK TARGET - This will be hooked via NPT view-switching
BOOL APIENTRY hwglSwapBuffers(HDC hDc) {
    static bool first_call = true;
    if (first_call) {
        glewExperimental = GL_TRUE;
        if (glewInit() == GLEW_OK) {
            g_GlewInitialized = true;
            initShaders();
        }
        first_call = false;
    }

    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    std::vector<glm::mat4> matrices_to_render;
    glm::mat4 currentViewMatrix;
    glm::mat4 currentProjectionMatrix;
    bool matrices_are_valid = false;
    {
        std::lock_guard<std::mutex> lock(g_MatrixMutex);
        matrices_to_render = g_GlobalModelMatrices;
        g_GlobalModelMatrices.clear();
        if (g_CurrentRenderingContext && g_ContextDataMap.find(g_CurrentRenderingContext) != g_ContextDataMap.end()) {
            ContextData& ctx_data = g_ContextDataMap[g_CurrentRenderingContext];
            currentViewMatrix = ctx_data.viewMatrix;
            currentProjectionMatrix = ctx_data.projectionMatrix;
            matrices_are_valid = ctx_data.matrices_valid;
        }
    }

    std::vector<glm::mat4> filtered_matrices_to_render;
    if (matrices_are_valid) {
        filtered_matrices_to_render = FilterMatricesByDistance(matrices_to_render, currentViewMatrix, 2.0f);
    }
    else {
        filtered_matrices_to_render = matrices_to_render;
    }

    if (g_ShaderProgram != 0 && !filtered_matrices_to_render.empty() && matrices_are_valid) {
        for (const auto& modelMatrix : filtered_matrices_to_render) {
            draw3DCube(modelMatrix, currentViewMatrix, currentProjectionMatrix, 1.0f);
        }
    }

    // Render 3918 entities
    {
        std::vector<glm::mat4> matrices3918;
        {
            std::lock_guard<std::mutex> lock(g_MatrixMutex);
            matrices3918.swap(g_GlobalModelMatrices3918);
        }
        for (const auto& modelMatrix : matrices3918) {
            draw3DCube3918(modelMatrix, currentViewMatrix, currentProjectionMatrix, 1.0f);
        }
    }

    glDisable(GL_LINE_SMOOTH);
    glPopAttrib();

    CurrentFramePoints.swap(NextFramePoints);
    NextFramePoints.clear();

    // Call original via VEH
    return veh::CallOriginal<BOOL>(reinterpret_cast<BOOL(WINAPI*)(HDC)>(g_Original_wglSwapBuffers), hDc);
}

void APIENTRY mglDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (count > 0) {
        std::lock_guard<std::mutex> lock(g_MatrixMutex);
        glm::mat4 invView = glm::inverse(SaveViewMatrix);
        glm::vec3 cameraPos = glm::vec3(invView[3]);
        for (size_t i = 0; i < SaveModelMatrix.size(); i++) {
            const glm::mat4& model = SaveModelMatrix[i];
            glm::vec3 modelPos = glm::vec3(model[3]);
            float distance = glm::distance(cameraPos, modelPos);
            if (distance <= 4.0f) continue;
            g_GlobalModelMatrices.push_back(model);
        }
        SaveModelMatrix.clear();
    }

    if (g_glDrawElements) {
        g_glDrawElements(mode, count, type, indices);
    }

    HGLRC current_context = wglGetCurrentContext();
    if (current_context == 0) return;
    g_CurrentRenderingContext = current_context;
    InitializeSSBOForContext(current_context);
    ContextData& ctx_data = g_ContextDataMap[current_context];
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, ctx_data.ssbo);

    if (aa) {
        aa = false;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx_data.ssbo);
        size_t offset_view = 0;
        size_t offset_proj = sizeof(glm::mat4);
        size_t offset_model_start = 2 * sizeof(glm::mat4);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, offset_view, sizeof(glm::mat4), &ctx_data.viewMatrix);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, offset_proj, sizeof(glm::mat4), &ctx_data.projectionMatrix);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, offset_model_start, sizeof(glm::mat4), ctx_data.matrices.data());
        {
            std::lock_guard<std::mutex> lock(g_MatrixMutex);
            glm::mat4 modelMatrix = ctx_data.matrices[0];
            glm::mat4 invViewMatrix = glm::inverse(ctx_data.viewMatrix);
            glm::vec3 cameraPosWorld = glm::vec3(invViewMatrix[3]);
            glm::vec3 modelPosWorld = glm::vec3(modelMatrix[3]);
            float distance = glm::length(modelPosWorld - cameraPosWorld);
            if (distance >= 2.0f) {
                g_GlobalModelMatrices.push_back(modelMatrix);
            }
            ctx_data.matrices_valid = true;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    if (count == 3918) {
        glm::mat4 modelMatrix;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ctx_data.ssbo);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 2 * sizeof(glm::mat4), sizeof(glm::mat4), &modelMatrix);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        {
            std::lock_guard<std::mutex> lock(g_MatrixMutex);
            glm::mat4 invViewMatrix = glm::inverse(ctx_data.viewMatrix);
            glm::vec3 cameraPosWorld = glm::vec3(invViewMatrix[3]);
            glm::vec3 modelPosWorld = glm::vec3(modelMatrix[3]);
            float distance = glm::length(modelPosWorld - cameraPosWorld);
            if (distance >= 2) {
                g_GlobalModelMatrices3918.push_back(modelMatrix);
            }
        }
    }
}

void APIENTRY mglUniform3fv(GLint location, GLsizei count, const GLfloat* value) {
    if (count > 10 && location) {
        aa = true;
    }
    if (g_glUniform3fv) {
        g_glUniform3fv(location, count, value);
    }
}

void APIENTRY mglUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &ProgramPlayers);
    ProjectionMatrixId = glGetUniformLocation(ProgramPlayers, "projMatrix");
    if (location == ProjectionMatrixId) {
        SaveProjectionMatrix = *reinterpret_cast<glm::mat4*>(const_cast<float*>(value));
    }
    ViewMatrixId = glGetUniformLocation(ProgramPlayers, "viewMatrix");
    if (location == ViewMatrixId) {
        SaveViewMatrix = *reinterpret_cast<glm::mat4*>(const_cast<float*>(value));
    }
    ModelMatrixId = glGetUniformLocation(ProgramPlayers, "modelMatrix");
    if (location == ModelMatrixId) {
        SaveModelMatrix.push_back(*reinterpret_cast<glm::mat4*>(const_cast<float*>(value)));
    }

    if (g_glUniformMatrix4fv) {
        g_glUniformMatrix4fv(location, count, transpose, value);
    }
}

void APIENTRY mglGetQueryObjectuiv(GLuint id, GLenum pname, GLuint* params) {
    if (g_glGetQueryObjectuiv) {
        g_glGetQueryObjectuiv(id, pname, params);
    }
    if (pname == GL_QUERY_RESULT_AVAILABLE) *params = GL_TRUE;
    else if (pname == GL_QUERY_RESULT) *params = 1;
}

void APIENTRY mglGetQueryObjectiv(GLuint id, GLenum pname, GLint* params) {
    if (g_glGetQueryObjectiv) {
        g_glGetQueryObjectiv(id, pname, params);
    }
    if (pname == GL_QUERY_RESULT_AVAILABLE) *params = GL_TRUE;
    else if (pname == GL_QUERY_RESULT) *params = 1;
}

std::string ModifyShaderIfMatch(const std::string& originalSource) {
    if (originalSource.find("#version") == std::string::npos) return originalSource;
    if (originalSource.find("layout (std140, column_major) uniform persistentBlock") == std::string::npos) return originalSource;
    if (originalSource.find("mat4 modelMatrix = mat4(") == std::string::npos) return originalSource;

    std::string modifiedSource = originalSource;

    size_t versionPos = modifiedSource.find("#version");
    if (versionPos != std::string::npos) {
        size_t endOfVersionLine = modifiedSource.find('\n', versionPos);
        if (endOfVersionLine != std::string::npos) {
            modifiedSource.erase(versionPos, endOfVersionLine - versionPos);
            modifiedSource.insert(versionPos, "#version 430\n");
        }
    }

    size_t persistentBlockEnd = modifiedSource.find("};", modifiedSource.find("layout (std140, column_major) uniform persistentBlock"));
    if (persistentBlockEnd != std::string::npos) {
        persistentBlockEnd += 2;
        std::string ssboDecl = "\nlayout(std430, binding = 0) buffer MatrixBuffer {\n"
            "    mat4 viewMatrixSSBO;\n"
            "    mat4 projectionMatrixSSBO;\n"
            "    mat4 modelMatrixSSBO;\n"
            "};\n";
        modifiedSource.insert(persistentBlockEnd, ssboDecl);
    }

    size_t modelMatrixAssignPos = modifiedSource.find("mat4 modelMatrix = mat4(");
    if (modelMatrixAssignPos != std::string::npos) {
        size_t endOfAssignLine = modifiedSource.find(';', modelMatrixAssignPos);
        if (endOfAssignLine != std::string::npos) {
            modifiedSource.insert(endOfAssignLine + 1, "\n    modelMatrixSSBO = modelMatrix;");
        }
    }

    size_t glPositionPos = modifiedSource.find("gl_Position = projectionMatrix * vertexPos_camera;");
    if (glPositionPos != std::string::npos) {
        size_t endOfGlPosLine = modifiedSource.find(';', glPositionPos);
        if (endOfGlPosLine != std::string::npos) {
            std::string writeViewProj = "\n    viewMatrixSSBO = viewMatrix;\n    projectionMatrixSSBO = projectionMatrix;";
            modifiedSource.insert(endOfGlPosLine + 1, writeViewProj);
        }
    }

    return modifiedSource;
}

void APIENTRY mglShaderSource(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)
{
    GLint shaderType;
    glGetShaderiv(shader, GL_SHADER_TYPE, &shaderType);

    if (shaderType != GL_VERTEX_SHADER) {
        if (g_glShaderSource) {
            g_glShaderSource(shader, count, string, length);
        }
        return;
    }

    if (count <= 0 || string == nullptr) {
        if (g_glShaderSource) {
            g_glShaderSource(shader, count, string, length);
        }
        return;
    }

    std::string shaderSource;
    if (length != nullptr) {
        for (GLsizei i = 0; i < count; ++i) {
            if (string[i] != nullptr && length[i] > 0) {
                shaderSource.append(string[i], length[i]);
            }
            else if (string[i] != nullptr) {
                shaderSource.append(string[i]);
            }
        }
    }
    else {
        for (GLsizei i = 0; i < count; ++i) {
            if (string[i] != nullptr) {
                shaderSource.append(string[i]);
            }
        }
    }

    std::string modifiedSource = ModifyShaderIfMatch(shaderSource);
    const char* modifiedCStr = modifiedSource.c_str();
    GLsizei modifiedLength = static_cast<GLsizei>(modifiedSource.length());

    if (g_glShaderSource) {
        g_glShaderSource(shader, 1, &modifiedCStr, &modifiedLength);
    }
}

// INLINE HOOK TARGET - wglGetProcAddress
PROC APIENTRY hwglGetProcAddress(LPCSTR ProcName)
{
    typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);
    PFN_wglGetProcAddress pOriginal = (PFN_wglGetProcAddress)g_Original_wglGetProcAddress;
    PROC realProc = pOriginal(ProcName);

    if (!strcmp(ProcName, "glDrawElements")) {
        static bool initialized = false;
        if (!initialized && realProc) {
            g_glDrawElements = reinterpret_cast<decltype(g_glDrawElements)>(realProc);
            initialized = true;
        }
        return (PROC)mglDrawElements;
    }
    else if (!strcmp(ProcName, "glUniform3fv")) {
        static bool initialized = false;
        if (!initialized && realProc) {
            g_glUniform3fv = reinterpret_cast<decltype(g_glUniform3fv)>(realProc);
            initialized = true;
        }
        return (PROC)mglUniform3fv;
    }
    else if (!strcmp(ProcName, "glUniformMatrix4fv")) {
        static bool initialized = false;
        if (!initialized && realProc) {
            g_glUniformMatrix4fv = reinterpret_cast<decltype(g_glUniformMatrix4fv)>(realProc);
            initialized = true;
        }
        return (PROC)mglUniformMatrix4fv;
    }
    else if (!strcmp(ProcName, "glGetQueryObjectuiv")) {
        static bool initialized = false;
        if (!initialized && realProc) {
            g_glGetQueryObjectuiv = reinterpret_cast<decltype(g_glGetQueryObjectuiv)>(realProc);
            initialized = true;
        }
        return (PROC)mglGetQueryObjectuiv;
    }
    else if (!strcmp(ProcName, "glGetQueryObjectiv")) {
        static bool initialized = false;
        if (!initialized && realProc) {
            g_glGetQueryObjectiv = reinterpret_cast<decltype(g_glGetQueryObjectiv)>(realProc);
            initialized = true;
        }
        return (PROC)mglGetQueryObjectiv;
    }
    else if (!strcmp(ProcName, "glShaderSource")) {
        static bool initialized = false;
        if (!initialized && realProc) {
            g_glShaderSource = reinterpret_cast<decltype(g_glShaderSource)>(realProc);
            initialized = true;
        }
        return (PROC)mglShaderSource;
    }

    return realProc;
}

// Uses VEH hooks - can work standalone or NPT will re-hook
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);
     
        HMODULE hOpenGL = GetModuleHandle(L"opengl32.dll");
        if (!hOpenGL) return FALSE;

        void* addr_wglSwapBuffers = GetProcAddress(hOpenGL, "wglSwapBuffers");
        void* addr_wglGetProcAddress = GetProcAddress(hOpenGL, "wglGetProcAddress");
        if (!addr_wglSwapBuffers || !addr_wglGetProcAddress) {
            return FALSE;
        }

        g_Original_wglSwapBuffers = addr_wglSwapBuffers;
        g_Original_wglGetProcAddress = addr_wglGetProcAddress;

        // Setup VEH for hooks
        veh::Setup();

        // Install hooks
        if (!veh::Hook(addr_wglSwapBuffers, reinterpret_cast<void*>(hwglSwapBuffers))) {
            return FALSE;
        }
        if (!veh::Hook(addr_wglGetProcAddress, reinterpret_cast<void*>(hwglGetProcAddress))) {
            return FALSE;
        }

        break;
    }
    case DLL_PROCESS_DETACH: {
        break;
    }
    }
    return TRUE;
}