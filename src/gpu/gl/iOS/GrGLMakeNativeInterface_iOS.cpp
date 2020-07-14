/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <cstdio>

//需要转存的最小的纹理大小,支持从服务器下发该值以控制该功能的影响范围。
#ifdef _DEBUG
static unsigned g_minSizeOfTexture = 0;
#else//_DEBUG
static unsigned g_minSizeOfTexture = 0;
#endif //_DEBUG
static bool g_minSizeOfTexture_setted = false;

#define MAX_SIZE_OF_TEXTURE 1024*1024*100

__attribute__((visibility("default"))) extern "C" void  setMinSizeOfTexture(unsigned int minSizeOfTexture)
{
    if (g_minSizeOfTexture_setted == true)
    {
        return;
    }
    g_minSizeOfTexture_setted = true;
    printf("flutter log—— setMinSizeOfTexture %d \n", minSizeOfTexture);
    g_minSizeOfTexture = minSizeOfTexture;
}

//一旦使用延迟纹理重建技术，纹理重建只发生在纹理需要绘制之前，不绘制的纹理将不会重建，这可以进一步节省内存，但是难点在于是否能找到所有绘制前的重建时机
#ifdef _DEBUG
static bool g_bisUseDelayedTextureRebuild = true;
#else//_DEBUG
static bool g_bisUseDelayedTextureRebuild = true;
#endif //_DEBUG
static bool g_bisUseDelayedTextureRebuild_setted = false;

__attribute__((visibility("default"))) extern "C" void  setUseDelayedTextureRebuild(bool isUseDelayedTextureRebuild)
{
    if (g_bisUseDelayedTextureRebuild_setted == true)
    {
        return;
    }
    g_bisUseDelayedTextureRebuild_setted = true;
    printf("flutter log—— setUseDelayedTextureRebuild %d \n", isUseDelayedTextureRebuild);
    g_bisUseDelayedTextureRebuild = isUseDelayedTextureRebuild;
}

bool g_bisBackground = false;
__attribute__((visibility("default"))) extern "C" void  setBackground(bool isBackground)
{
    printf("flutter log—— setBackground %d \n", isBackground);
    g_bisBackground = isBackground;
}


#include "include/core/SkTypes.h"
#ifdef SK_BUILD_FOR_IOS

#include "include/gpu/gl/GrGLInterface.h"

#include "include/gpu/gl/GrGLAssembleInterface.h"
#include "include/private/SkTemplates.h"

#include <dlfcn.h>
#include <memory>

#include "include/private/SkMutex.h"
//#include <unistd.h>
#include <pthread.h>

#define USE_FILEMMAP 1
#ifdef USE_FILEMMAP
#include "sys/mman.h"
static void *_fileMmap(size_t mmap_size, GrGLuint textureID);
#endif

#define GR_GL_COLOR_ATTACHMENT0              0x8CE0
#define GR_GL_TEXTURE_2D                     0x0DE1
#define GR_GL_RGBA                           0x1908
#define GR_GL_UNSIGNED_BYTE                  0x1401

//TODO: niudongsheng: 1, only reconstruct the used bmp. done
//TODO: niudongsheng: 2, muti level texture neccesary?
//TODO: niudongsheng: 4, find a better place to add this logic:there may be a little leak because the save logic is prior the draw logic before viewdestroy
//TODO: niudongsheng: 5, is there some texure is not RGBA internalformat?, reconstruction use RGBA cause the memory boom.
//TODO: niudongsheng: 6, the code in formatSupportsTexStorage may cause some problem, plz reduce the modification.
//TODO: niudongsheng: 7, 删除纹理对应的内存的方式，除了设置为宽高为1的像素点，还有其它方法吗？
//TODO: niudongsheng: 8, filemmap的执行性能问题
//TODO: niudongsheng: 9, change the location of code into native code can reduce the cost of maintainance.
//TODO: niudongsheng: 10, not all the gl allcation is catched, such as the function texsubimage2d glclear texstorage2d.
//TODO: niudongsheng: 11, readpixel read 32 bit, we may reduce it to 565 format.
//TODO: niudongsheng: 12, use compressedTexImage2D can reduce memory consumption? is there some way to reduce the file mmap size or prevent dump to files.
//DONE: niudongsheng: 13, use rgba4 may have too low background quality in film stuio. use rgba5A1 cause blink of progress bar of film studio.
//TODO: niudongsheng: 14, can we just reduce the picture quality of pictures which has more details?
//TODO: niudongsheng: 15, can we only trigger svae and clear on lowmemorynotification.

#define TEXTURE_POOL_MAX_SIZE 300
struct sTexture_Pool
{
    GrGLuint textureID;
    GrGLenum target;
    GrGLint level;
    GrGLint internalformat;
    GrGLsizei width;
    GrGLsizei height;
    GrGLint border;
    GrGLenum format;
    GrGLenum type;
    GrGLvoid* pixels;
    #ifdef USE_FILEMMAP
    GrGLint size;
    #endif
} g_sTexture_Pool[TEXTURE_POOL_MAX_SIZE] = {};
static SkMutex mutex_g_sTexture_Pool;

#ifdef _DEBUG
struct sTexturePixelChecker_Pool
{
    char pixelsForCheck[16];
} g_sTexturePixelChecker_Pool[TEXTURE_POOL_MAX_SIZE] = {};
#endif

#if defined(__aarch64__)
static __thread GrGLuint g_curTextureID = 0;
//#elif defined(__x86_64__)
//static __thread GrGLuint g_curTextureID = 0;
#else
static GrGLuint g_curTextureID = 0;
#endif

#ifdef _DEBUG
bool b_has_saved = false;
int64_t gpu_thread_id = 0;//对于即将要删除的纹理，没必要保存。所以需要检查是不是有纹理的删除是发生在save之后。
#endif

typedef GrGLvoid (*F_BindTexture_original)(GrGLenum target, GrGLuint texture);
static GrGLvoid (*fBindTexture_original)(GrGLenum target, GrGLuint texture) = nullptr;
static inline void restoreTextureWithContext(GrGLuint dstTexture);
static GrGLvoid glBindTextures_hooker(GrGLenum target, GrGLuint texture)
{
    SkAutoMutexExclusive lock(mutex_g_sTexture_Pool);
#ifdef _DEBUG
    b_has_saved = false;
    //fprintf(stdout, "=======fBindTexture_original [%d]threadid=[%lld]====\n", texture, (int64_t)pthread_self());
#endif
    (*fBindTexture_original)(target, texture);
    if (texture != 0 && g_bisUseDelayedTextureRebuild)
    {
        restoreTextureWithContext(texture);
    }
    g_curTextureID = texture;
}

//GrGLvoid GR_GL_FUNCTION_TYPE(GrGLsizei n, GrGLuint* textures);
typedef GrGLvoid (*F_GenTextures_original)(GrGLsizei n, GrGLuint* textures);
static GrGLvoid (*fGenTextures_original)(GrGLsizei n, GrGLuint* textures) = nullptr;
static GrGLvoid glGenTextures_hooker(GrGLsizei n, GrGLuint* textures)
{
    SkASSERT(n==1);
    (*fGenTextures_original)(n, textures);
    //fprintf(stdout, "===glGenTextures_hooker + generated id=[%d]====\n", *textures);
}

//GrGLvoid GR_GL_FUNCTION_TYPE(GrGLsizei n, GrGLuint* framebuffers);
typedef GrGLvoid (*F_GenFramebuffers_original)(GrGLsizei n, GrGLuint* framebuffers);
static GrGLvoid (*fGenFramebuffers_original)(GrGLsizei n, GrGLuint* framebuffers) = nullptr;
static GrGLvoid glGenFramebuffers_hooker(GrGLsizei n, GrGLuint* framebuffers)
{
    (*fGenFramebuffers_original)(n, framebuffers);
    //fprintf(stdout, "===glGenFramebuffers_hooker + generated id=[%d]====\n", *framebuffers);
}

//GrGLvoid GR_GL_FUNCTION_TYPE(GrGLenum target, GrGLuint framebuffer);
typedef GrGLvoid (*F_BindFramebuffer_original)(GrGLenum target, GrGLuint framebuffer);
static GrGLvoid (*fBindFramebuffer_original)(GrGLenum target, GrGLuint framebuffer) = nullptr;
static GrGLvoid glBindFramebuffer_hooker(GrGLenum target, GrGLuint framebuffer)
{
    (*fBindFramebuffer_original)(target, framebuffer);
    //fprintf(stdout, "===glBindFramebuffer_hooker  id=[%d]====\n", framebuffer);
}

//GrGLvoid GR_GL_FUNCTION_TYPE(GrGLenum target, GrGLenum attachment, GrGLenum textarget, GrGLuint texture, GrGLint level);
typedef GrGLvoid (*F_FramebufferTexture2D_original)(GrGLenum target, GrGLenum attachment, GrGLenum textarget, GrGLuint texture, GrGLint level);
static GrGLvoid (*fFramebufferTexture2D_original)(GrGLenum target, GrGLenum attachment, GrGLenum textarget, GrGLuint texture, GrGLint level) = nullptr;
static GrGLvoid glFramebufferTexture2D_hooker(GrGLenum target, GrGLenum attachment, GrGLenum textarget, GrGLuint texture, GrGLint level)
{
    (*fFramebufferTexture2D_original)(target, attachment, textarget, texture, level);
    //fprintf(stdout, "===glFramebufferTexture2D_hooker +  id=[%d]====\n", texture);
}

//using GrGLDeleteFramebuffersFn = GrGLvoid GR_GL_FUNCTION_TYPE(GrGLsizei n, const GrGLuint* framebuffers);
typedef GrGLvoid (*F_DeleteFramebuffers_original)(GrGLsizei n, const GrGLuint* framebuffers);
static GrGLvoid (*fDeleteFramebuffers_original)(GrGLsizei n, const GrGLuint* framebuffers) = nullptr;
static GrGLvoid glDeleteFramebuffers_hooker(GrGLsizei n, const GrGLuint* framebuffers)
{
    (*fDeleteFramebuffers_original)(n, framebuffers);
    //fprintf(stdout, "===glDeleteFramebuffers_hooker + id=[%d]====\n", *framebuffers);
}

typedef GrGLvoid (*F_Finish_original)();
static GrGLvoid (*fFinish_original)() = nullptr;
static GrGLvoid glFinish_hooker()
{
    (*fFinish_original)();
    //fprintf(stdout, "===glFinish_hooker \n");
}

typedef GrGLvoid (*F_Flush_original)();
static GrGLvoid (*fFlush_original)() = nullptr;
static GrGLvoid glFlush_hooker()
{
    (*fFlush_original)();
    //fprintf(stdout, "===glFlush_hooker \n");
}


//GrGLTexImage2DFn = GrGLvoid GR_GL_FUNCTION_TYPE(GrGLenum target, GrGLint level, GrGLint internalformat, GrGLsizei width, GrGLsizei height, GrGLint border, GrGLenum format, GrGLenum type, const GrGLvoid* pixels);
typedef GrGLvoid (*F_TexImage2D_original)(GrGLenum target, GrGLint level, GrGLint internalformat, GrGLsizei width, GrGLsizei height, GrGLint border, GrGLenum format, GrGLenum type, const GrGLvoid* pixels);
static GrGLvoid (*fTexImage2D_original)(GrGLenum target, GrGLint level, GrGLint internalformat, GrGLsizei width, GrGLsizei height, GrGLint border, GrGLenum format, GrGLenum type, const GrGLvoid* pixels) = nullptr;
static GrGLvoid glTexImage2D_hooker(GrGLenum target, GrGLint level, GrGLint internalformat, GrGLsizei width, GrGLsizei height, GrGLint border, GrGLenum format, GrGLenum type, const GrGLvoid* pixels)
{
    SkAutoMutexExclusive lock(mutex_g_sTexture_Pool);
    if (g_minSizeOfTexture >= MAX_SIZE_OF_TEXTURE) return;

    if (target!=GR_GL_TEXTURE_2D) return;

    (*fTexImage2D_original)(target, level, internalformat, width, height, border, format, type, pixels);

    //无论是否比最小texture小，都需要做清理操作，以免有把纹理从大于阈值更新到小于阈值的情况
    if (g_curTextureID < TEXTURE_POOL_MAX_SIZE)
    {
        if (g_sTexture_Pool[g_curTextureID].pixels != nullptr)
        {
            #ifdef USE_FILEMMAP
            munmap(g_sTexture_Pool[g_curTextureID].pixels,g_sTexture_Pool[g_curTextureID].size);
            g_sTexture_Pool[g_curTextureID].size = 0;
            #else
            free(g_sTexture_Pool[g_curTextureID].pixels);
            #endif
            g_sTexture_Pool[g_curTextureID].pixels = nullptr;
            fprintf(stdout, "===!!!! glTexImage2D_hooker:   leak data\n");
        }
        //如果其它level过来的时候做了清零，会导致记载的0levle信息因为产生了其它层次的绘制而被清除。
        if (level == 0)
        {
            g_sTexture_Pool[g_curTextureID] = {};
        }
    }

    if ( g_minSizeOfTexture < (unsigned int)(width * height * 4) && g_curTextureID < TEXTURE_POOL_MAX_SIZE )
    {
        //TODO niudongsheng: all level should be noted and rebuild.
        if (level == 0)
        {
            #ifdef USE_FILEMMAP
            g_sTexture_Pool[g_curTextureID] = {g_curTextureID, target, level, internalformat, width, height, border, format, type, nullptr, 0};
            #else
            g_sTexture_Pool[g_curTextureID] = {g_curTextureID, target, level, internalformat, width, height, border, format, type, nullptr};
            #endif
        }
        else
        {
            // 前面清零了，这里的逻辑判断会误判。
            // if (!(g_sTexture_Pool[g_curTextureID].width >= width && g_sTexture_Pool[g_curTextureID].height >= height))
            // {
            //     //fprintf(stdout, "===!!!glTexImage2D_hooker += malloc memory id=[%d] target=[%d] level=[%d] internalformat=[%x] width=[%d] height=[%d] border=[%d] format=[%x] type=[%x]====\n",
            //     //g_curTextureID, target, level, internalformat, width, height, border, format, type);
            //     //TODO: other level size bigger than level 0, why? what we should to do?
            //     fprintf(stdout, "====***glTexImage2D_hooker level=[%d], g_sTexture_Pool[%d].width=[%d], this width=[%d], g_sTexture_Pool[%d].height=[%d], height=[%d]\n",
            //         level,g_curTextureID,g_sTexture_Pool[g_curTextureID].width,width,g_curTextureID,g_sTexture_Pool[g_curTextureID].height,height);
            // }
        }
    }
    
//    fprintf(stdout, "===glTexImage2D_hooker += malloc memory id=[%d] target=[%d] level=[%d] internalformat=[%x] width=[%d] height=[%d] border=[%d] format=[%x] type=[%x]====\n",
//    g_curTextureID, target, level, internalformat, width, height, border, format, type);
}
#ifdef _DEBUG
void print()
{
          fprintf(stdout, "====!!! glDeleteTextures_hooker - the id is saved but deleted imediatly :id=[%d] threadid=[%lld]====\n", 0, (int64_t)pthread_self());
}
#endif

//GrGLvoid GR_GL_FUNCTION_TYPE(GrGLsizei n, const GrGLuint* textures);
typedef GrGLvoid (*F_DeleteTextures_original)(GrGLsizei n, const GrGLuint* textures);
static GrGLvoid (*fDeleteTextures_original)(GrGLsizei n, const GrGLuint* textures) = nullptr;
static GrGLvoid glDeleteTextures_hooker(GrGLsizei n, const GrGLuint* textures)
{
    SkAutoMutexExclusive lock(mutex_g_sTexture_Pool);
    if (g_minSizeOfTexture >= MAX_SIZE_OF_TEXTURE) return;
#ifdef _DEBUG
    //fprintf(stdout, "=======glDeleteTextures_hooker - deleted id=[%d] threadid=[%lld]====\n", *textures, (int64_t)pthread_self());
    if (b_has_saved && (int64_t)pthread_self() == gpu_thread_id)
    {
        print();
    }
#endif
    SkASSERT(n==1);
    if (*textures < TEXTURE_POOL_MAX_SIZE) {
        if (g_sTexture_Pool[*textures].pixels != nullptr)
        {
            #ifdef USE_FILEMMAP
            munmap(g_sTexture_Pool[*textures].pixels,g_sTexture_Pool[*textures].size);
            g_sTexture_Pool[*textures].size = 0;
            #else
            free(g_sTexture_Pool[*textures].pixels);
            #endif
            //usleep(1000*100);
            g_sTexture_Pool[*textures].pixels = nullptr;
            //fprintf(stdout, "===!!!! glDeleteTextures_hooker:   leak data id=[%d]\n", g_sTexture_Pool[*textures].textureID);
            g_sTexture_Pool[*textures] = {};
            //fprintf(stdout, "===!!!! glDeleteTextures_hooker2:   leak data id=[%d]\n", g_sTexture_Pool[*textures].textureID);
            //fprintf(stdout, "=======glDeleteTextures_hooker end - deleted id=[%d]====\n", *textures);
        }
        g_sTexture_Pool[*textures] = {};
    }
    (*fDeleteTextures_original)(n, textures);
}

//GrGLvoid GR_GL_FUNCTION_TYPE(GrGLint x, GrGLint y, GrGLsizei width, GrGLsizei height, GrGLenum format, GrGLenum type, GrGLvoid* pixels);
typedef GrGLvoid (*F_ReadPixels_original)(GrGLint x, GrGLint y, GrGLsizei width, GrGLsizei height, GrGLenum format, GrGLenum type, GrGLvoid* pixels);
static GrGLvoid (*fReadPixels_original)(GrGLint x, GrGLint y, GrGLsizei width, GrGLsizei height, GrGLenum format, GrGLenum type, GrGLvoid* pixels) = nullptr;
static GrGLvoid glReadPixels_hooker(GrGLint x, GrGLint y, GrGLsizei width, GrGLsizei height, GrGLenum format, GrGLenum type, GrGLvoid* pixels)
{
    if (g_bisBackground) {
        fprintf(stdout, "===glReadPixels_hooker - background====\n");
        return;
    }
    (*fReadPixels_original)(x, y, width, height, format, type, pixels);
}

static void storePixels(char* pixels, GrGLuint src_texture, int width, int height)
{
//    GLuint fbo;
//
//    glGenFramebuffers(1, &fbo);
//
//    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
//
//    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src_id, 0);
//
//    glBindTexture(GL_TEXTURE_2D, dest_id);
//
//    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,  M_WIDTH ,M_HEIGHT);
//
//    glBindFramebuffer(GL_FRAMEBUFFER, 0);
//    glBindTexture(GL_TEXTURE_2D, 0);
    //glReadPixels 是从framebuffers里面读，而不是texture，但是可以借助FBO，可以绑定到FBO，然后从FBO用glReadPixels读取

    GrGLuint offscreen_fbo;
    //GLuint src_texture;
//    int width = 100;
//    int height = 100;
    #define GR_GL_FRAMEBUFFER                    0x8D40
    (*fGenFramebuffers_original)(1, &offscreen_fbo);
    (*fBindFramebuffer_original)(GR_GL_FRAMEBUFFER, offscreen_fbo);
    



    //创建tex

    
//    GLubyte* src_pixels = (GLubyte*) malloc(width * height * sizeof(GLubyte) * 4);
//    memset(src_pixels, 133, width * height * sizeof(GLubyte) * 4);
//
//    glGenTextures(1, &src_texture);
//    glBindTexture(GL_TEXTURE_2D, src_texture);
//    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, src_pixels);
//    free(src_pixels);
//    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
//    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);


    //glBindFramebuffer(GL_FRAMEBUFFER, offscreen_fbo);

    (*fBindTexture_original)(GR_GL_TEXTURE_2D, src_texture);
    (*fFramebufferTexture2D_original)(GR_GL_FRAMEBUFFER, GR_GL_COLOR_ATTACHMENT0, GR_GL_TEXTURE_2D, src_texture, 0);
    //glViewport(0, 0, width, height);
    //GLubyte pixels[100][100][4] = {};
    //memset(pixels, 0, width * height * sizeof(GLubyte) * 4);
#ifdef _DEBUG
    #define MAGIC_NUMBER_CHECK_READPIXEL  0x7F8D1C5A
    *((int*)pixels) = MAGIC_NUMBER_CHECK_READPIXEL;
#endif
    (*fReadPixels_original)(0, 0, width, height, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, pixels);
#ifdef _DEBUG
    //read the original pixels.
    (*fReadPixels_original)(0, 0, 1, 1, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, g_sTexturePixelChecker_Pool[src_texture].pixelsForCheck);

    if (*((int*)pixels) == MAGIC_NUMBER_CHECK_READPIXEL)
    {
        //fprintf(stdout,"=====!!!! storePixels failed to readpixels with texture ++ID=[%d] width=[%d], height=[%d]\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);
        // for (int i = 0; i< width; i++)
        // {
            fprintf(stdout, "=====!!!! storePixels failed to readpixels with texture [%d]: pixels[%d]=[%x]\n", src_texture, 0,  *((int*)pixels));
        // }
    }
    else
    {
        // for (int i = 0; i< width; i++)
        // {
        //     fprintf(stdout, "===== storePixels success to readpixels with texture [%d]: pixels[%d]=[%d]\n", src_texture, i,  *((char*)pixels +i));
        // }
    }
#endif//#ifdef _DEBUG
 
    (*fBindFramebuffer_original)(GR_GL_FRAMEBUFFER, 0);
    (*fBindTexture_original)(GR_GL_TEXTURE_2D, 0);
    (*fDeleteFramebuffers_original)(1, &offscreen_fbo);
    //glDeleteTextures(1, &src_texture);
    
}

void grGLClearAndSaveAllTextures()
{
    SkAutoMutexExclusive lock(mutex_g_sTexture_Pool);
#ifdef _DEBUG
    gpu_thread_id = (int64_t)pthread_self();
    b_has_saved = true;
    fprintf(stdout, "===grGLClearAndSaveAllTextures_Relase - g_minSizeOfTexture =[%d] threadid=[%lld]====\n", g_minSizeOfTexture, (int64_t)pthread_self());
#endif
    
    if (g_minSizeOfTexture >= MAX_SIZE_OF_TEXTURE) return;

    int count_all_textures = 0;
    int count_saved_textures = 0;
    for (GrGLuint i = 1; i< TEXTURE_POOL_MAX_SIZE; i++)
    {
        if (g_sTexture_Pool[i].textureID == i )
        {
            count_all_textures++;
            if (g_sTexture_Pool[i].pixels == nullptr && g_sTexture_Pool[i].width >0 && g_sTexture_Pool[i].height>0)
            {

                //tao lu
                //GL_CALL(TexParameteri( GR_GL_TEXTURE_2D, GR_GL_TEXTURE_MIN_FILTER, GR_GL_LINEAR ));
                //GL_CALL(TexParameteri( GR_GL_TEXTURE_2D, GR_GL_TEXTURE_MAG_FILTER, GR_GL_LINEAR ));
                //GL_CALL(TexParameteri( GR_GL_TEXTURE_2D, GR_GL_TEXTURE_WRAP_S, GR_GL_CLAMP_TO_EDGE));
                //GL_CALL(TexParameteri( GR_GL_TEXTURE_2D, GR_GL_TEXTURE_WRAP_T, GR_GL_CLAMP_TO_EDGE));

                // if (g_sTexture_Pool[i].pixels != nullptr)
                // {
                //     //free(g_sTexture_Pool[i].pixels);
                //     fprintf(stdout, "===---- grGLClearAndSaveAllTextures:   already saved!");
                //     continue;
                // }
                #ifdef USE_FILEMMAP
                g_sTexture_Pool[i].pixels = (char*)_fileMmap(g_sTexture_Pool[i].width*g_sTexture_Pool[i].height*4, i);
                #else
                g_sTexture_Pool[i].pixels = (char*)malloc(g_sTexture_Pool[i].width*g_sTexture_Pool[i].height*4);
                #endif
                
                if (nullptr == g_sTexture_Pool[i].pixels) {
                    fprintf(stdout,"=====!!!! grGLClearAndSaveAllTextures failed to malloc with texture ++ID=[%d] width=[%d], height=[%d]\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);
                    continue;
                }
                #ifdef USE_FILEMMAP
                g_sTexture_Pool[i].size = g_sTexture_Pool[i].width*g_sTexture_Pool[i].height*4;
                #endif
                //memset(g_sTexture_Pool[i].pixels,0,g_sTexture_Pool[i].width*g_sTexture_Pool[i].height*4);
                storePixels((char*)g_sTexture_Pool[i].pixels, i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);

                int fw = 1, fh = 1;
                static char spriteData[16];
                //memset(spriteData, 0, 16);
#ifdef _DEBUG
                fprintf(stdout, "===grGLClearAndSaveAllTextures - clear id=[%d]width=[%d]height=[%d]====\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);
#endif
                (*fBindTexture_original)(g_sTexture_Pool[i].target, i);
                //TODO niudongsheng: fixme multi level needs more reconstruction.
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 0, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 1, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 2, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 3, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 4, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 5, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 6, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 7, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 8, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 9, GR_GL_RGBA, fw, fh, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, spriteData);
               count_saved_textures++;
            }
        }
    }
    fprintf(stdout, "===grGLClearAndSaveAllTextures - count_all_textures =[%d] , count_saved_textures =[%d]\n", count_all_textures, count_saved_textures);
}

static inline void restoreTextureWithContext(GrGLuint dstTexture)
{
    GrGLuint i = dstTexture;
    if (g_sTexture_Pool[i].textureID == i)
    {
        if (nullptr != g_sTexture_Pool[i].pixels && g_sTexture_Pool[i].width >0 && g_sTexture_Pool[i].height>0 )
        {
            (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 0, g_sTexture_Pool[i].internalformat, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, g_sTexture_Pool[i].pixels);
            #ifdef USE_FILEMMAP
            munmap(g_sTexture_Pool[i].pixels,g_sTexture_Pool[i].size);
            g_sTexture_Pool[i].size = 0;
            #else
            free(g_sTexture_Pool[i].pixels);
            #endif
            g_sTexture_Pool[i].pixels = nullptr;
            #ifdef _DEBUG
            fprintf(stdout, "===restoreTextureWithContext + restored id=[%d]width=[%d]height=[%d]====\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);
            #endif//#ifdef _DEBUG
        }
    }
}

static bool restoreTexture(GrGLuint dstTexture)
{
    GrGLuint i = dstTexture;
    if (g_sTexture_Pool[i].textureID == i)
    {
        if (nullptr != g_sTexture_Pool[i].pixels && g_sTexture_Pool[i].width >0 && g_sTexture_Pool[i].height>0 )
        {
            //this is debug code. leave me here for future debug.
//                char * pixels =(char*) g_sTexture_Pool[i].pixels;
//                if (*((char*)pixels +1 ) == *((char*)pixels +2 ) && *((char*)pixels) == (char)133)
//                {
//                    //fprintf(stdout,"=====!!!! storePixels failed to readpixels with texture ++ID=[%d] width=[%d], height=[%d]\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);
//
//                }
//                else
//                {
//                    for (int j = 0; j< g_sTexture_Pool[i].width; j++)
//                    {
//                        fprintf(stdout, "=====!!!! grGLClearAndSaveAllTextures pixels was changed with texture [%d]: pixels[%d]=[%d]\n", g_sTexture_Pool[j].textureID, j,  *((char*)pixels +j));
//                    }
//                }

                {
                    GrGLuint offscreen_fbo;
                    //GLuint src_texture;
                //    int width = 100;
                //    int height = 100;
                    #define GR_GL_FRAMEBUFFER                    0x8D40
                    (*fGenFramebuffers_original)(1, &offscreen_fbo);
                    (*fBindFramebuffer_original)(GR_GL_FRAMEBUFFER, offscreen_fbo);
                    



                    //创建tex

                    
                //    GLubyte* src_pixels = (GLubyte*) malloc(width * height * sizeof(GLubyte) * 4);
                //    memset(src_pixels, 133, width * height * sizeof(GLubyte) * 4);
                //
                //    glGenTextures(1, &src_texture);
                //    glBindTexture(GL_TEXTURE_2D, src_texture);
                //    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, src_pixels);
                //    free(src_pixels);
                //    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                //    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);


                    //glBindFramebuffer(GL_FRAMEBUFFER, offscreen_fbo);

                    (*fBindTexture_original)(GR_GL_TEXTURE_2D, i);
                    (*fFramebufferTexture2D_original)(GR_GL_FRAMEBUFFER, GR_GL_COLOR_ATTACHMENT0, GR_GL_TEXTURE_2D, i, 0);
                    //glViewport(0, 0, width, height);

                    //this is debug code. leave me here for future debug.
                    //memset(g_sTexture_Pool[i].pixels, 255, g_sTexture_Pool[i].width * g_sTexture_Pool[i].height *4);
                    if (g_sTexture_Pool[i].pixels==nullptr)
                    {
                        fprintf(stdout, "===!!!!  restoreTexture who released me?\n");
                    }
#ifdef _DEBUG
                    {//check the pixels before restore.
                        if (memcmp(g_sTexture_Pool[i].pixels,g_sTexturePixelChecker_Pool[i].pixelsForCheck, 4) != 0)
                        {
                            //fprintf(stdout,"=====!!!! storePixels failed to readpixels with texture ++ID=[%d] width=[%d], height=[%d]\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);

                            fprintf(stdout, "=====!!! grGLrestoreAllTexures failed to restore pixel because mmap pixels is wrong origpixel=0x[%d] mmap pixels=[%d]========\n",
                            *(int*)(&g_sTexturePixelChecker_Pool[i].pixelsForCheck), *(int*)g_sTexture_Pool[i].pixels);
                        }
                    }

                    //memset(g_sTexture_Pool[i].pixels, 255, g_sTexture_Pool[i].width * g_sTexture_Pool[i].height * 4);
#endif//#ifdef _DEBUG

                    (*fTexImage2D_original)(GR_GL_TEXTURE_2D, 0, g_sTexture_Pool[i].internalformat, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height, 0, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, g_sTexture_Pool[i].pixels);
#ifdef _DEBUG
                    {//check the pixels after restore.
                        char pixelsForCheck[16] = {};

                        (*fReadPixels_original)(0, 0, 1, 1, GR_GL_RGBA, GR_GL_UNSIGNED_BYTE, pixelsForCheck);

                        if (memcmp(pixelsForCheck,g_sTexturePixelChecker_Pool[i].pixelsForCheck, 4) != 0)
                        {
                            //fprintf(stdout,"=====!!!! storePixels failed to readpixels with texture ++ID=[%d] width=[%d], height=[%d]\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);

                            fprintf(stdout, "=====!!! grGLrestoreAllTexures failed to restore pixel because failed to write to gpu, origpixel=0x[%d] restored pixels=[%d]========\n",
                            *(int*)(&g_sTexturePixelChecker_Pool[i].pixelsForCheck), *(int*)pixelsForCheck);
                        }
                    }
#endif//#ifdef _DEBUG
                    (*fBindFramebuffer_original)(GR_GL_FRAMEBUFFER, 0);
                    (*fBindTexture_original)(GR_GL_TEXTURE_2D, 0);
                    (*fDeleteFramebuffers_original)(1, &offscreen_fbo);
                    
                }

            #ifdef USE_FILEMMAP
            munmap(g_sTexture_Pool[i].pixels,g_sTexture_Pool[i].size);
            g_sTexture_Pool[i].size = 0;
            #else
            free(g_sTexture_Pool[i].pixels);
            #endif
            g_sTexture_Pool[i].pixels = nullptr;
            fprintf(stdout, "===restoreTexture + restored id=[%d]width=[%d]height=[%d]====\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);
            return true;
        }
        else if (g_sTexture_Pool[i].width <=0 || g_sTexture_Pool[i].height<=0)
        {
            fprintf(stdout, "===!!!restoreTexture but width or height is 0 + restored id=[%d]width=[%d]height=[%d]====\n", i, g_sTexture_Pool[i].width, g_sTexture_Pool[i].height);
        }
    }
    return false;
}

void grGLRestoreAllTexures()
{
   SkAutoMutexExclusive lock(mutex_g_sTexture_Pool);
   if (g_minSizeOfTexture >= MAX_SIZE_OF_TEXTURE) return;

   if (g_bisUseDelayedTextureRebuild) return;

   //fprintf(stdout, "===grGLRestoreAllTexures: g_minSizeOfTexture = [%d] threadid=[%lld]====\n", g_minSizeOfTexture, (int64_t)pthread_self());

   int count_all_textures = 0;
   int count_restored_textures = 0;
   for (GrGLuint i = 1; i< TEXTURE_POOL_MAX_SIZE; i++)
   {
        if (g_sTexture_Pool[i].textureID == i)
        {
            count_all_textures++;
            if (true == restoreTexture(i))
            {
                count_restored_textures++;
            }
        }
   }

    fprintf(stdout, "===grGLrestoreAllTexures ++ count_all_textures= [%d] , count_restored_textures= [%d]\n",count_all_textures,count_restored_textures );
}


sk_sp<const GrGLInterface> GrGLMakeNativeInterface() {
    grGLRestoreAllTexures();

    static const char kPath[] =
        "/System/Library/Frameworks/OpenGL.framework/Versions/A/Libraries/libGL.dylib";
    std::unique_ptr<void, SkFunctionWrapper<int(void*), dlclose>> lib(dlopen(kPath, RTLD_LAZY));
    return GrGLMakeAssembledGLESInterface(lib.get(), [](void* ctx, const char* name) {
        
        if (strcmp("glReadPixels", name) == 0)
        {
            fReadPixels_original = (F_ReadPixels_original) dlsym(fLoader.handle(), name);
            return (GrGLFuncPtr) &glReadPixels_hooker;
        }
        else if (strcmp("glFinish", name) == 0)
        {
            fFinish_original = (F_Finish_original) dlsym(fLoader.handle(), name);
            return (GrGLFuncPtr) &glFinish_hooker;
        }
        else if (strcmp("glFlush", name) == 0)
        {
            fFlush_original = (F_Flush_original) dlsym(fLoader.handle(), name);
            return (GrGLFuncPtr) &glFlush_hooker;
        }

        if (g_minSizeOfTexture < MAX_SIZE_OF_TEXTURE)
        {
            if (strcmp("glGenTextures",name) == 0)
            {
                fGenTextures_original = (F_GenTextures_original) dlsym(fLoader.handle(), name);
                return (GrGLFuncPtr) &glGenTextures_hooker;
            }
            else if (strcmp("glTexImage2D", name) == 0)
            {
                fTexImage2D_original = (F_TexImage2D_original) dlsym(fLoader.handle(), name);
                return (GrGLFuncPtr) &glTexImage2D_hooker;
            }
            else if (strcmp("glBindTexture", name) == 0)
            {
                fBindTexture_original = (F_BindTexture_original) dlsym(fLoader.handle(), name);
                return (GrGLFuncPtr) &glBindTextures_hooker;
            }
            else if (strcmp("glDeleteTextures", name) == 0)
            {
                fDeleteTextures_original = (F_DeleteTextures_original) dlsym(fLoader.handle(), name);
                return (GrGLFuncPtr) &glDeleteTextures_hooker;
            }
            else if (strcmp("glGenFramebuffers", name)==0)
            {//
                fGenFramebuffers_original = (F_GenFramebuffers_original) dlsym(fLoader.handle(), name);
                return (GrGLFuncPtr) &glGenFramebuffers_hooker;
            }
            else if (strcmp("glBindFramebuffer", name)==0)
            {
                fBindFramebuffer_original = (F_BindFramebuffer_original) dlsym(fLoader.handle(), name);
                return (GrGLFuncPtr) &glBindFramebuffer_hooker;
            }
            else if (strcmp("glFramebufferTexture2D", name)==0)
            {
                fFramebufferTexture2D_original = (F_FramebufferTexture2D_original) dlsym(fLoader.handle(), name);
                return (GrGLFuncPtr) &glFramebufferTexture2D_hooker;
            }
            else if (strcmp("glDeleteFramebuffers", name)==0)
            {
                fDeleteFramebuffers_original = (F_DeleteFramebuffers_original) dlsym(fLoader.handle(), name);
                return (GrGLFuncPtr) &glDeleteFramebuffers_hooker;
            }
        }

            return (GrGLFuncPtr)dlsym(ctx ? ctx : RTLD_DEFAULT, name); });
}

const GrGLInterface* GrGLCreateNativeInterface() { return GrGLMakeNativeInterface().release(); }


#ifdef USE_FILEMMAP
#include "sys/mman.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if defined HAVE_STDINT_H
#include <stdint.h>
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>
#else
#include <sys/types.h>
#endif
#include <unistd.h>

#define FILENAME_MAX_LENGTH 512

static char g_fileNamePrefixOfTextureCache[FILENAME_MAX_LENGTH];
static bool s_isInited_fileMmap = false;
bool sk_mkdir(const char* path);


//caution: do not use this function in multithread.
static void *_fileMmap(size_t mmap_size, GrGLuint textureID)
{

    if (mmap_size > MAX_SIZE_OF_TEXTURE)
    {
        fprintf(stdout, "===!!!_fileMmap failed to mmap too large file size=[%lu].\n", mmap_size);
        return nullptr;
    }
    char *ptr = NULL;
    if (!s_isInited_fileMmap)
    {
        char *home = getenv("HOME");
        snprintf(g_fileNamePrefixOfTextureCache,FILENAME_MAX_LENGTH,"%s/%s/",home+strlen("/private"),"tmp/texture_cache");
        sk_mkdir(g_fileNamePrefixOfTextureCache);
        s_isInited_fileMmap = true;
    }
    char fileNameWhole[FILENAME_MAX_LENGTH];
    snprintf(fileNameWhole,FILENAME_MAX_LENGTH,"%s/%d",g_fileNamePrefixOfTextureCache,textureID);

    FILE *fp = fopen (fileNameWhole, "wb+") ;
    bool isFailed = false;
    
    if(fp != NULL){
           int ret = ftruncate(fileno(fp), mmap_size);
           if(ret == -1){
               isFailed = true;
           }
           else {
               fseek(fp, 0, SEEK_SET);
               ptr = (char *)mmap(0, mmap_size, PROT_WRITE | PROT_READ, (MAP_FILE|MAP_SHARED), fileno(fp), 0);
            //    if (ptr !=MAP_FAILED ) {
            //        memset(ptr, 0, mmap_size);
            //    }
//               if(ptr != NULL){
//                   memset(ptr, '1', mmap_size);
//                   for (size_t count = 0; count < mmap_size; count++)
//                   {
//                       *(ptr+count)='2';
//                   }
//               }
//               else {
//                   isFailed = true;
//               }
           }
       }
       else {
           isFailed = true;
       }
    
    //munmap(ptr,mmap_size);
    fclose(fp);
    if (isFailed || ptr == MAP_FAILED)
    {
        fprintf(stdout, "===!!!_fileMmap failed to mmap for textureID= [%d] , mmap_size= [%lu]\n", textureID, mmap_size);
        return (void*)(nullptr);
    }
    return (void*)(ptr);
}


//munmap(const_cast<void*>(addr), length);

#endif//USE_FILEMMAP


#endif  // SK_BUILD_FOR_IOS
