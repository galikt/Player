#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <SDL2/SDL.h>
#include "SDL2/SDL_thread.h"

#define AUDIO_BUFFER_SIZE 4096
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue
{
  AVPacketList *first_pkt;
  AVPacketList *last_pkt;
  int nb_packets;
  int size;
  SDL_mutex *mutex;
  SDL_cond *cond;
} PacketQueue;

typedef struct
{
  AVCodecContext *pCodecContext;
  AVCodec        *pCodec;
  int             StreamID;
} Decoder;

typedef struct
{
  AVFormatContext   *pFormatContext;
  struct SwsContext *SWSContext;
  Decoder            VideoDecoder;
  Decoder            AudioDecoder;

  struct Audio
  {
    SDL_AudioSpec Spec;
    PacketQueue Queue;
  };
  struct Audio Audio;

  struct Window
  {
    SDL_Window   *WindowHandle;
    SDL_Texture  *Texture;
    SDL_Renderer *Renderer;
    int           Width;
    int           Heigth;
    SDL_Rect      ViewPort;
  };
  struct Window Window;

  struct Frame
  {
    int       UVPitch;
    AVFrame  *pFrame;
    AVPicture Picture
  };
  struct Frame Frame;

} Context;
//********************************************************************************

//Добавление пакетов в очередь
int QueuePush(PacketQueue *Queue, AVPacket *Packet)
{
  if (av_dup_packet(Packet) < 0)
    return -1;
  
  AVPacketList *PacketList = av_malloc(sizeof(AVPacketList));
  if (PacketList == NULL)
    return -1;

  PacketList->pkt = *Packet;
  PacketList->next = NULL;

  SDL_LockMutex(Queue->mutex);
  if (!Queue->last_pkt)
    Queue->first_pkt = PacketList;
  else
    Queue->last_pkt->next = PacketList;

  Queue->last_pkt = PacketList;
  Queue->nb_packets++;
  Queue->size += PacketList->pkt.size;  
  SDL_UnlockMutex(Queue->mutex);

  SDL_CondSignal(Queue->cond);

  return 0;
}
//********************************************************************************

static int QueuePop(PacketQueue *Queue, AVPacket *Packet, int block)
{
  AVPacketList *PacketList;
  int ret;
  
  SDL_LockMutex(Queue->mutex);
  
  for(;;)
  {
    PacketList = Queue->first_pkt;
    if (PacketList)
    {
      Queue->first_pkt = PacketList->next;

      if (!Queue->first_pkt)
	      Queue->last_pkt = NULL;

      Queue->nb_packets--;
      Queue->size -= PacketList->pkt.size;
      *Packet = PacketList->pkt;
      av_free(PacketList);
      ret = 1;
      break;
    }
    else if (!block)
    {
      ret = 0;
      break;
    }
    else
    {
      SDL_CondWait(Queue->cond, Queue->mutex);
    }
  }
  SDL_UnlockMutex(Queue->mutex);

  return ret;
}
//********************************************************************************

void QueueInit(PacketQueue *Queue)
{
  memset(Queue, 0, sizeof(PacketQueue));
  Queue->mutex = SDL_CreateMutex();
  Queue->cond = SDL_CreateCond();
}
//********************************************************************************

int audio_decode_frame(Context *Context, uint8_t *audio_buf, int buf_size)
{
  AVCodecContext *pCodecContext = Context->AudioDecoder.pCodecContext;

  static AVPacket pkt;
  static uint8_t *audio_pkt_data = NULL;
  static int audio_pkt_size = 0;
  static AVFrame frame;

  int len1, data_size = 0;

  for(;;)
  {
    while (audio_pkt_size > 0)
    {
      int got_frame = 0;
      len1 = avcodec_decode_audio4(pCodecContext, &frame, &got_frame, &pkt);

      if (len1 < 0)
      {
	      audio_pkt_size = 0;
	      break;
      }

      audio_pkt_data += len1;
      audio_pkt_size -= len1;
      data_size = 0;

      if (got_frame)
      {
	      data_size = av_samples_get_buffer_size(
          NULL, 
					pCodecContext->channels,
					frame.nb_samples,
					pCodecContext->sample_fmt,
					1
        );
	      memcpy(audio_buf, frame.data[0], data_size);
      }
      if (data_size <= 0)
      {
	      continue;
      }
      return data_size;
    }

    if (pkt.data)
      av_free_packet(&pkt);

    if (QueuePop(&Context->Audio.Queue, &pkt, 1) < 0)
    {
      return -1;
    }

    audio_pkt_data = pkt.data;
    audio_pkt_size = pkt.size;
  }
}
//********************************************************************************

void AudioCallback(void *userdata, Uint8 *stream, int len)
{
  Context *Context = (struct Context*)userdata;
  AVCodecContext *pCodecContext = Context->AudioDecoder.pCodecContext;
  int len1, audio_size;

  static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
  static unsigned int audio_buf_size = 0;
  static unsigned int audio_buf_index = 0;

  while (len > 0)
  {
    if (audio_buf_index >= audio_buf_size)
    {
      audio_size = audio_decode_frame(Context, audio_buf, sizeof(audio_buf));
      if (audio_size < 0)
      {
      	audio_buf_size = 1024;
      	memset(audio_buf, 0, audio_buf_size);
      }
      else
      {
      	audio_buf_size = audio_size;
      }
        audio_buf_index = 0;
    }
    len1 = audio_buf_size - audio_buf_index;

    if(len1 > len)
      len1 = len;

    memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
    len -= len1;
    stream += len1;
    audio_buf_index += len1;
  }
}
//********************************************************************************

int InitAudio(Context *Context)
{
  const AVCodecContext const *pCodecContext = Context->AudioDecoder.pCodecContext;

  SDL_AudioSpec Spec;
  Spec.freq = pCodecContext->sample_rate;
  Spec.format = AUDIO_S16SYS;
  Spec.channels = pCodecContext->channels;
  Spec.silence = 0;
  Spec.samples = AUDIO_BUFFER_SIZE;
  Spec.callback = AudioCallback;
  Spec.userdata = Context;
  
  SDL_AudioSpec ResultSpec;
  if(SDL_OpenAudio(&Spec, &ResultSpec) < 0)
    return -1;

  Context->Audio.Spec = ResultSpec;

  QueueInit(&Context->Audio.Queue);
  SDL_PauseAudio(0);

  return 0;
}
//********************************************************************************

//Инициализация SDL, создание окна
int CreateWindow(Context *Context, const char *Name, int Width, int Heigth)
{
  SDL_DisplayMode DisplayMode;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
    return -1;

  if (SDL_GetDesktopDisplayMode(0, &DisplayMode) != 0)
    return -1;

  SDL_Window *WindowHandle = SDL_CreateWindow(Name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, Width, Heigth, SDL_WINDOW_SHOWN);
  if (WindowHandle == NULL)
    return -1;

  //Создание поверхности для отображения
  SDL_Surface *Screen = SDL_CreateRGBSurface(0, Width, Heigth, 32, 0, 0, 0, 0);
  if (Screen == NULL)
    return -1;

   SDL_Renderer *Renderer = SDL_CreateRenderer(WindowHandle, -1, SDL_RENDERER_ACCELERATED);
   if (Renderer == NULL)
      return -1;

  Context->Window.WindowHandle = WindowHandle;
  Context->Window.Width = Width;
  Context->Window.Heigth = Heigth;
  Context->Window.Renderer =  Renderer;

  return 0;
}
//********************************************************************************

//Уничтожение окна
void DestroyWindow(Context *Context)
{
  SDL_DestroyWindow(Context->Window.WindowHandle);
}
//********************************************************************************

//Запуск цикла сообщений SDL
void Run(Context *Context)
{
  SDL_Event event;

  for(;;)
  { 
    ReadFrame(Context);

    SDL_WaitEvent(&event);

    switch(event.type) 
    {
      case SDL_QUIT:
        CloseVideo(Context);
        DestroyWindow(Context);
        exit(0);
        break;
    }
  }
}
//********************************************************************************

void UpdateWindowSize(Context *Context)
{

}
//********************************************************************************

void UpdateViewPort(Context *Context)
{
  float AspectRation = (float)Context->VideoDecoder.pCodecContext->height / (float)Context->VideoDecoder.pCodecContext->width;
  int Width = Context->Window.Width;
  int Heigth = AspectRation * Width;
  int X = 0;
  int Y = (Context->Window.Heigth / 2) - (Heigth / 2);

  SDL_Rect ViewPort = { X, Y, Width, Heigth };
  Context->Window.ViewPort = ViewPort;
}
//********************************************************************************

int UpdateSurface(Context *Context)
{
  struct SwsContext *SWSContext = sws_getContext(
    Context->VideoDecoder.pCodecContext->width,
    Context->VideoDecoder.pCodecContext->height,
    Context->VideoDecoder.pCodecContext->pix_fmt,
    Context->Window.ViewPort.w,
    Context->Window.ViewPort.h,
    AV_PIX_FMT_YUV420P,
    SWS_BILINEAR,
    NULL,
    NULL,
    NULL
  );

  SDL_Texture *Texture = SDL_CreateTexture(
    Context->Window.Renderer,
    SDL_PIXELFORMAT_YV12,
    SDL_TEXTUREACCESS_STREAMING,
    Context->Window.ViewPort.w,
    Context->Window.ViewPort.h
  );
  if (Texture == NULL)
    return -1;

  Context->SWSContext = SWSContext;
  Context->Window.Texture = Texture;

  return 0;
}
//********************************************************************************

//Выделение памяти для кадра
int UpdateFrameSize(Context *Context)
{
  AVFrame *pFrame = av_frame_alloc();
  if (pFrame == NULL)
    return -1;

  const AVCodecContext const *pCodecContext = Context->VideoDecoder.pCodecContext;

  int uvPitch = pCodecContext->width / 2;
  size_t yPlaneSize = pCodecContext->width * pCodecContext->height;
  size_t uvPlaneSize = pCodecContext->width * pCodecContext->height / 4;
  Uint8 *yPlane = (Uint8*)malloc(yPlaneSize);
  Uint8 *uPlane = (Uint8*)malloc(uvPlaneSize);
  Uint8 *vPlane = (Uint8*)malloc(uvPlaneSize);
  if ((yPlane == 0) || (uPlane == 0) || (vPlane == 0))
    return -1;

  AVPicture *Picture = &Context->Frame.Picture;
  Picture->data[0] = yPlane;
  Picture->data[1] = uPlane;
  Picture->data[2] = vPlane;
  Picture->linesize[0] = pCodecContext->width;
  Picture->linesize[1] = uvPitch;
  Picture->linesize[2] = uvPitch;

  Context->Frame.pFrame = pFrame;
  Context->Frame.UVPitch = uvPitch;

  return 0;
}
//********************************************************************************

//Освобождения памяти кадра
void FreeMemoryFrame(Context *Context)
{
  av_frame_free(Context->Frame.pFrame);

  free(Context->Frame.Picture.data[0]);
  free(Context->Frame.Picture.data[1]);
  free(Context->Frame.Picture.data[2]);
}
//********************************************************************************

int OpenCodec(AVFormatContext *pFormatContext, Decoder *outDecoder, int StreamID)
{
  AVCodecContext *pCodecContextOrigin = pFormatContext->streams[StreamID]->codec;
  AVCodec *pCodec = avcodec_find_decoder(pCodecContextOrigin->codec_id);
  if (pCodec == NULL)
    return -1;

  //Копия контекста кодека.
  AVCodecContext *pCodecContext = NULL;
  pCodecContext = avcodec_alloc_context3(pCodec);
  if (avcodec_copy_context(pCodecContext, pCodecContextOrigin) != 0)
    return -1;

  //Открываем кодек
  if (avcodec_open2(pCodecContext, pCodec, NULL) < 0)
    return -1;

  outDecoder->pCodec = pCodec;
  outDecoder->pCodecContext = pCodecContext;
  outDecoder->StreamID = StreamID;

  return 0;
}
//********************************************************************************

//Инициализация библиотеки и открытие видеофайла
int OpenVideo(Context *Context, const char *FileName)
{
  av_register_all();

  AVFormatContext *pFormatContext = NULL;
  if (avformat_open_input(&pFormatContext, FileName, NULL, 0) != 0)
    return -1;

  if (avformat_find_stream_info(pFormatContext, NULL) < 0)
    return -1;

  av_dump_format(pFormatContext, 0, FileName, 0);

  //Поиск потоков
  int VideoStreamID = -1;
  int AudioStreamID = -1;
  for(int i = 0; i < pFormatContext->nb_streams; i++)
  {
    switch (pFormatContext->streams[i]->codec->codec_type)
    {
    case AVMEDIA_TYPE_VIDEO:
      if (VideoStreamID < 0)
        VideoStreamID = i;
      break;
    case AVMEDIA_TYPE_AUDIO:
      if (AudioStreamID < 0)
        AudioStreamID = i;
      break;    
    }
  }
  if (VideoStreamID == -1 || AudioStreamID == -1)
    return -1;

  Decoder VideoDecoder;
  if (OpenCodec(pFormatContext, &VideoDecoder, VideoStreamID) != 0)
    return -1;

  Decoder AudioDecoder;
  if (OpenCodec(pFormatContext, &AudioDecoder, AudioStreamID) != 0)
    return -1;

  Context->pFormatContext = pFormatContext;
  Context->VideoDecoder = VideoDecoder;
  Context->AudioDecoder = AudioDecoder;

  UpdateViewPort(Context);
  if ((UpdateSurface(Context) != 0) || (UpdateFrameSize(Context) != 0))
    return -1;

  return 0;
}
//********************************************************************************

//Закрытие файла, освобождение ресурсов библиотеки
void CloseVideo(Context *Context)
{
  FreeMemoryFrame(Context);
  
  avcodec_close(Context->VideoDecoder.pCodecContext);
  avformat_close_input(&Context->pFormatContext);
  sws_freeContext(Context->SWSContext);
}
//********************************************************************************

//Чтение кадра
int ReadFrame(Context *Context)
{
  AVCodecContext *pCodecContext = Context->VideoDecoder.pCodecContext;
  struct Frame *Frame = &Context->Frame;

  int FrameFinished;
  AVPacket Packet;
  while (av_read_frame(Context->pFormatContext, &Packet) >= 0)
  {
    if (Packet.stream_index == Context->VideoDecoder.StreamID)
    {
      avcodec_decode_video2(pCodecContext, Frame->pFrame, &FrameFinished, &Packet);
      
      if (FrameFinished)
      {
        sws_scale(
          Context->SWSContext,
          (uint8_t const * const *)Frame->pFrame->data,
          Frame->pFrame->linesize,
          0,
          pCodecContext->height,
          Context->Frame.Picture.data,
          Context->Frame.Picture.linesize
        );
        DrawFrame(Context);
      }
    }
    else if (Packet.stream_index == Context->AudioDecoder.StreamID)
    {
      QueuePush(&Context->Audio.Queue, &Packet);
    }
    else
    {
      av_free_packet(&Packet);
    }

    //av_free_packet(&Packet);
  }

  return 0;
}
//********************************************************************************

void DrawFrame(Context *Context)
{
  SDL_UpdateYUVTexture(
    Context->Window.Texture,
    NULL,
    Context->Frame.Picture.data[0],
    Context->VideoDecoder.pCodecContext->width,
    Context->Frame.Picture.data[1],
    Context->Frame.UVPitch,
    Context->Frame.Picture.data[2],
    Context->Frame.UVPitch
   );

  SDL_RenderClear(Context->Window.Renderer);
  SDL_RenderCopy(Context->Window.Renderer, Context->Window.Texture, NULL, &Context->Window.ViewPort);
  SDL_RenderPresent(Context->Window.Renderer);
}
//********************************************************************************

int main(int argc, char *argv[])
{
  Context Context;

  if (CreateWindow(&Context, "Player", 800, 600) != 0)
    return -1;


  if (OpenVideo(&Context, argv[1]) != 0)
    return -1;

  InitAudio(&Context);
  
  Run(&Context);

  return 0;
}
//********************************************************************************