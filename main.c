#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <naomi/video.h>
#include <naomi/audio.h>
#include <naomi/maple.h>
#include <naomi/eeprom.h>
#include <naomi/thread.h>
#include <naomi/interrupt.h>
#include <naomi/romfs.h>
#include <xmp.h>

#define BUFSIZE 8192
#define SAMPLERATE 44100

void *audiothread_main(void *param)
{
    char *details = param;

    xmp_context ctx = xmp_create_context();

    if (xmp_load_module(ctx, "rom://noel.xm") < 0)
    {
        ATOMIC(strcpy(details, "Failed to open XM file!"));
        return 0;
    }
    else if (xmp_start_player(ctx, SAMPLERATE, 0) != 0)
    {
        ATOMIC(strcpy(details, "Failed to start XML file!"));
        xmp_release_module(ctx);
    }
    else
    {
        struct xmp_module_info mi;
        struct xmp_frame_info fi;

        xmp_get_module_info(ctx, &mi);
        ATOMIC(sprintf(details, "%s (%s)", mi.mod->name, mi.mod->type));

        while (xmp_play_frame(ctx) == 0)
        {
            xmp_get_frame_info(ctx, &fi);
            unsigned int numsamples = fi.buffer_size / 4;
            uint32_t *samples = (uint32_t *)fi.buffer;

            ATOMIC(sprintf(details, "%s (%s) - %3d/%3d %3d/%3d", mi.mod->name, mi.mod->type, fi.pos, mi.mod->len, fi.row, fi.num_rows));
            while (numsamples > 0)
            {
                unsigned int actual_written = audio_write_stereo_data(samples, numsamples);
                if (actual_written < numsamples)
                {
                    numsamples -= actual_written;
                    samples += actual_written;

                    // Sleep for the time it takes to play half our buffer so we can wake up and
                    // fill it again.
                    thread_sleep((int)(1000000.0 * (((float)BUFSIZE / 4.0) / (float)SAMPLERATE)));
                }
                else
                {
                    numsamples = 0;
                }
            }
        }

        xmp_end_player(ctx);
        xmp_release_module(ctx);
    }

    xmp_free_context(ctx);
    return 0;
}
void main()
{
    // Get settings so we know how many controls to read.
    eeprom_t settings;
    eeprom_read(&settings);

    // Initialize some crappy video.
    video_init_simple();
    video_set_background_color(rgb(48, 48, 48));

    // Initialize the ROMFS.
    romfs_init_default();

    // Initialize audio system.
    audio_init();
    audio_register_ringbuffer(AUDIO_FORMAT_16BIT, SAMPLERATE, BUFSIZE);

    // Load our module to play it.
    char details[1024];
    memset(details, 0, 1024);

    uint32_t audiothread = thread_create("audio", &audiothread_main, details);
    thread_priority(audiothread, 1);
    thread_start(audiothread);

    unsigned int counter = 0;
    while ( 1 )
    {
        // Display info about playback.
        video_draw_debug_text(20, 20, rgb(255, 255, 255), details);

        // Display a liveness counter that goes up 60 times a second.
        video_draw_debug_text(
            20,
            40,
            rgb(200, 200, 20),
            "Aliveness counter: %d (%lu)",
            counter++,
            audio_aica_uptime()
        );

        // Wait for vblank and draw it!
        video_display_on_vblank();
    }
}

void test()
{
    video_init_simple();

    while ( 1 )
    {
        video_fill_screen(rgb(48, 48, 48));
        video_draw_debug_text(320 - 56, 236, rgb(255, 255, 255), "test mode stub");
        video_display_on_vblank();
    }
}
