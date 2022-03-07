
/*
 * PulseAudio host to play natively in Linux based systems without
 * ALSA emulation
 *
 * Copyright (c) 2022 Tuukka Pasanen <tuukka.pasanen@ilmi.fi>
 *
 * Based on the Open Source API proposed by Ross Bencina
 * Copyright (c) 1999-2002 Ross Bencina, Phil Burk
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The text above constitutes the entire PortAudio license; however,
 * the PortAudio community also makes the following non-binding requests:
 *
 * Any person wishing to distribute modifications to the Software is
 * requested to send the modifications to the original developer so that
 * they can be incorporated into the canonical version. It is also
 * requested that these non-binding requests be included along with the
 * license above.
 */

/** @file
 @ingroup common_src

 @brief PulseAudio implementation of support for a host API.

 This host API implements PulseAudio support for portaudio
 it has callback mode and normal write mode support
*/

#include "pa_util.h"
#include "pa_allocation.h"
#include "pa_hostapi.h"
#include "pa_stream.h"
#include "pa_cpuload.h"
#include "pa_process.h"

#include "pa_unix_util.h"
#include "pa_ringbuffer.h"

#include "pa_linux_pulseaudio_cb_internal.h"

#include <pthread.h>

/* PulseAudio headers */
#include <string.h>
#include <unistd.h>

int PaPulseAudio_writeAudio(PaPulseAudio_Stream *stream, int length)
{
    uint8_t l_cBUffer[PULSEAUDIO_BUFFER_SIZE];
    pa_operation *l_ptrOperation = NULL;

    memset(l_cBUffer, 0x00, length);

    /* Write everything and hope for the best */
    if(length <= 0)
    {
        length = PaUtil_GetRingBufferReadAvailable( &stream->outputRing );
    }

    PaUtil_ReadRingBuffer( &stream->outputRing,
                           l_cBUffer,
                           length );

    pa_threaded_mainloop_lock( stream->mainloop );
    if( pa_stream_write( stream->outStream,
                         l_cBUffer,
                         length,
                         NULL,
                         0,
                         PA_SEEK_RELATIVE) )
    {
        PA_DEBUG( ("Portaudio %s: Can't write audio!\n",
                  __FUNCTION__) );
        return paCanNotWriteToACallbackStream;
    }
    pa_threaded_mainloop_unlock( stream->mainloop );

    return paNoError;
}

void *PaPulseAudio_processThread( void *userdata )
{
    PaPulseAudio_Stream *l_ptrStream = (PaPulseAudio_Stream *) userdata;
    PaStreamCallbackTimeInfo timeInfo = { 0, 0, 0 };
    size_t l_lBufferSize = 0;
    const void *l_ptrSampleData = NULL;
    size_t l_lOutFrameBytes = 0;
    size_t l_lInFrameBytes = 0;
    int l_iResult = paContinue;
    long numFrames = 0;
    int i = 0;
    uint8_t l_cBUffer[PULSEAUDIO_BUFFER_SIZE];
    uint32_t l_lFramesPerHostBuffer = l_ptrStream->bufferProcessor.framesPerHostBuffer;
    size_t l_lWritableBytes = 0;
    size_t l_lReadableBytes = 0;

    int l_bOutputCb = 0;
    int l_bInputCb = 0;

    pa_usec_t l_lStreamLatency = 0;
    unsigned int l_iNegative = 0;

    const pa_buffer_attr *l_SBufferAttr = NULL;



    if( l_ptrStream->inStream )
    {
        if( l_ptrStream->bufferProcessor.framesPerHostBuffer == paFramesPerBufferUnspecified )
        {
            l_SBufferAttr = pa_stream_get_buffer_attr( l_ptrStream->inStream  );
            l_lFramesPerHostBuffer = l_SBufferAttr->tlength / l_ptrStream->inputFrameSize;
        }

        l_lInFrameBytes = (l_lFramesPerHostBuffer * l_ptrStream->inputFrameSize);
        if( l_ptrStream->bufferProcessor.streamCallback )
        {
            l_bInputCb = 1;
        }

    }

    if( l_ptrStream->outStream )
    {
        if( !l_lFramesPerHostBuffer && l_ptrStream->bufferProcessor.framesPerHostBuffer == paFramesPerBufferUnspecified )
        {
            l_SBufferAttr = pa_stream_get_buffer_attr( l_ptrStream->outStream  );
            l_lFramesPerHostBuffer = l_SBufferAttr->tlength / l_ptrStream->outputFrameSize;
        }

        l_lOutFrameBytes = (l_lFramesPerHostBuffer * l_ptrStream->outputFrameSize);
        if( l_ptrStream->bufferProcessor.streamCallback )
        {
            l_bOutputCb = 1;
        }
    }

    do {
        // pa_threaded_mainloop_wait(l_ptrStream->mainloop);
        PaUtil_BeginCpuLoadMeasurement( &l_ptrStream->cpuLoadMeasurer );

        if( l_ptrStream->outStream )
        {
                pa_threaded_mainloop_lock( l_ptrStream->mainloop );
                l_lWritableBytes = pa_stream_writable_size( l_ptrStream->outStream );
                pa_threaded_mainloop_unlock( l_ptrStream->mainloop );
        }

        if( l_ptrStream->inStream )
        {
                pa_threaded_mainloop_lock( l_ptrStream->mainloop );
                l_lReadableBytes = pa_stream_readable_size( l_ptrStream->inStream );
                pa_threaded_mainloop_unlock( l_ptrStream->mainloop );
        }



        // if( (l_bOutputCb || l_bInputCb) &&
        //     l_ptrStream->bufferProcessor.framesPerHostBuffer == paFramesPerBufferUnspecified )
        // {
        //     if( l_ptrStream->outStream )
        //     {
        //         if( l_lWritableBytes )
        //         {
        //             l_lOutFrameBytes = l_lInFrameBytes = l_lWritableBytes * l_ptrStream->outputFrameSize;
        //             l_lTmpFramesPerHostBuffer = l_lOutFrameBytes / l_ptrStream->outputFrameSize;
        //             // printf("YEA\n");
        //         }
        //     }
        //     if( l_ptrStream->inStream )
        //     {
        //         if( l_lReadableBytes )
        //         {
        //             l_lInFrameBytes = l_lInFrameBytes = l_lReadableBytes * l_ptrStream->inputFrameSize;
        //             l_lTmpFramesPerHostBuffer = l_lInFrameBytes /  l_ptrStream->inputFrameSize;
        //         }
        //     }
        //
        //     // if( l_lTmpFramesPerHostBuffer > l_lFramesPerHostBuffer )
        //     // {
        //     //     l_lFramesPerHostBuffer = l_lTmpFramesPerHostBuffer;
        //     // }
        //     //
        //     // if( l_lFramesPerHostBuffer < 16 )
        //     // {
        //     //     l_lFramesPerHostBuffer = 16;
        //     //     l_lInFrameBytes = l_lInFrameBytes = l_lFramesPerHostBuffer * l_ptrStream->inputFrameSize;
        //     // }
        //
        //     l_lTmpFramesPerHostBuffer = 364;
        //     l_lFramesPerHostBuffer = l_lTmpFramesPerHostBuffer;
        //     l_lOutFrameBytes = l_lFramesPerHostBuffer * l_ptrStream->outputFrameSize;
        // }

        if( l_ptrStream->inStream )
        {
            PaPulseAudio_updateTimeInfo( l_ptrStream->inStream,
                                         &timeInfo,
                                         1 );
        }

        if( l_ptrStream->outStream )
        {
            PaPulseAudio_updateTimeInfo( l_ptrStream->outStream,
                                         &timeInfo,
                                         0 );
        }

        if( ( l_bOutputCb &&
            ( PaUtil_GetRingBufferReadAvailable( &l_ptrStream->outputRing ) < ( l_lOutFrameBytes * 3 ) ||
            ( l_lWritableBytes && PaUtil_GetRingBufferReadAvailable( &l_ptrStream->outputRing ) < l_lWritableBytes ) )) ||
            (l_bInputCb &&
            PaUtil_GetRingBufferReadAvailable( &l_ptrStream->inputRing ) >= l_lInFrameBytes) )
        {

            PaUtil_BeginBufferProcessing( &l_ptrStream->bufferProcessor,
                                          &timeInfo,
                                          0 );

            /* Read of ther is something to read */
            if( l_bInputCb )
            {
                if(PaUtil_GetRingBufferReadAvailable(&l_ptrStream->inputRing) >= l_lInFrameBytes)
                {
                    PaUtil_ReadRingBuffer(&l_ptrStream->inputRing,
                                          l_cBUffer,
                                          l_lInFrameBytes);
                }
                else
                {
                    memset(l_cBUffer, 0x00, l_lInFrameBytes);
                }

                PaUtil_SetInterleavedInputChannels( &l_ptrStream->bufferProcessor,
                                                    0,
                                                    l_cBUffer,
                                                    l_ptrStream->inSampleSpec.channels );

                PaUtil_SetInputFrameCount( &l_ptrStream->bufferProcessor,
                                           l_lFramesPerHostBuffer );
            }

            if( l_bOutputCb )
            {
                PaUtil_SetInterleavedOutputChannels( &l_ptrStream->bufferProcessor,
                                                     0,
                                                     l_cBUffer,
                                                     l_ptrStream->outputChannelCount );
                PaUtil_SetOutputFrameCount( &l_ptrStream->bufferProcessor,
                                            l_lFramesPerHostBuffer );

                /* If mono we assume to have stereo output
                * So we just copy to other channel..
                * Little bit hackish but works.. with float currently
                */
                if( l_ptrStream->outputChannelCount == 1 )
                {
                    void *l_ptrStartOrig = l_cBUffer + l_lOutFrameBytes;
                    void *l_ptrStartStereo = l_cBUffer;
                    memcpy(l_ptrStartOrig, l_ptrStartStereo, l_lOutFrameBytes);

                    for(i = 0; i < l_lOutFrameBytes; i += l_ptrStream->outputFrameSize)
                    {
                        memcpy( l_ptrStartStereo,
                                l_ptrStartOrig,
                                l_ptrStream->outputFrameSize );
                        l_ptrStartStereo += l_ptrStream->outputFrameSize;
                        memcpy( l_ptrStartStereo,
                                l_ptrStartOrig,
                                l_ptrStream->outputFrameSize );
                        l_ptrStartStereo += l_ptrStream->outputFrameSize;
                        l_ptrStartOrig += l_ptrStream->outputFrameSize;
                    }

                    memcpy(l_ptrStartStereo, l_ptrStartOrig, l_lOutFrameBytes);
                }

                PaUtil_WriteRingBuffer( &l_ptrStream->outputRing,
                                        l_cBUffer,
                                        l_lOutFrameBytes );
            }

            numFrames =
                PaUtil_EndBufferProcessing( &l_ptrStream->bufferProcessor,
                                            &l_iResult );
        }

        if( l_bOutputCb )
        {
            if( l_lWritableBytes > 0 &&
                l_lWritableBytes < PaUtil_GetRingBufferReadAvailable( &l_ptrStream->outputRing ) )
            {

                PaPulseAudio_writeAudio(l_ptrStream, l_lWritableBytes);
            }
        }

        if( l_ptrStream->inStream )
        {
            pa_threaded_mainloop_lock( l_ptrStream->mainloop );
            if( l_lReadableBytes > 0 )
            {
                l_ptrSampleData = NULL;

                if( pa_stream_peek( l_ptrStream->inStream,
                                    &l_ptrSampleData,
                                    &l_lReadableBytes ))
                {
                    PA_DEBUG( ("Portaudio %s: Can't read audio!\n",
                              __FUNCTION__) );
                }
                else
                {
                    PaUtil_WriteRingBuffer( &l_ptrStream->inputRing,
                                            l_ptrSampleData,
                                            l_lReadableBytes);
                    /* XXX should check whether all bytes were actually written */
                }

                pa_stream_drop( l_ptrStream->inStream );
            }
            pa_threaded_mainloop_unlock( l_ptrStream->mainloop );
        }
        PaUtil_EndCpuLoadMeasurement( &l_ptrStream->cpuLoadMeasurer,
                                      numFrames );

        if( l_iResult != paContinue )
        {
            /* Eventually notify user all buffers have played */
        	if( l_ptrStream->streamRepresentation.streamFinishedCallback
              && l_ptrStream->isActive )
        	{
        	   l_ptrStream->streamRepresentation.streamFinishedCallback( l_ptrStream->streamRepresentation.userData );
        	}

            pa_threaded_mainloop_lock( l_ptrStream->mainloop );
            l_ptrStream->isActive = 0;
            pa_threaded_mainloop_unlock( l_ptrStream->mainloop );
            break;
        }

        usleep(200);
    } while( l_ptrStream->isActive );

    if( l_bOutputCb )
    {
        PaUtil_FlushRingBuffer( &l_ptrStream->outputRing );
    }

    if( l_bInputCb )
    {
        PaUtil_FlushRingBuffer( &l_ptrStream->inputRing );
    }

    l_ptrStream->threadActive = 0;

    pthread_exit( NULL );

    return NULL;
}
