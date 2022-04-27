#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>
#include <naomi/video.h>
#include <naomi/audio.h>
#include <naomi/maple.h>
#include <naomi/eeprom.h>
#include <naomi/thread.h>
#include <naomi/interrupt.h>
#include <naomi/romfs.h>
#include <naomi/timer.h>
#include <xmp.h>
#include <timidity.h>

#define BUFSIZE 8192
#define SAMPLERATE 44100

typedef struct
{
    char filename[1024];
    char modulename[128];
    char tracker[128];
    char position[128];
    volatile int exit;
    volatile int error;
    uint32_t thread;
} audiothread_instructions_t;

void *audiothread_xmp(void *param)
{
    audiothread_instructions_t *instructions = (audiothread_instructions_t *)param;

    xmp_context ctx = xmp_create_context();

    if (xmp_load_module(ctx, instructions->filename) < 0)
    {
        instructions->error = 1;
        return 0;
    }
    else if (xmp_start_player(ctx, SAMPLERATE, 0) != 0)
    {
        instructions->error = 2;
        xmp_release_module(ctx);
    }
    else
    {
        struct xmp_module_info mi;
        struct xmp_frame_info fi;

        xmp_get_module_info(ctx, &mi);
        ATOMIC(strcpy(instructions->modulename, mi.mod->name));
        ATOMIC(strcpy(instructions->tracker, mi.mod->type));

        audio_register_ringbuffer(AUDIO_FORMAT_16BIT, SAMPLERATE, BUFSIZE);

        while (xmp_play_frame(ctx) == 0 && instructions->exit == 0)
        {
            xmp_get_frame_info(ctx, &fi);
            int numsamples = fi.buffer_size / 4;
            uint32_t *samples = (uint32_t *)fi.buffer;

            ATOMIC(sprintf(instructions->position, "%3d/%3d %3d/%3d", fi.pos, mi.mod->len, fi.row, fi.num_rows));
            while (numsamples > 0)
            {
                int actual_written = audio_write_stereo_data(samples, numsamples);
                if (actual_written < 0)
                {
                    instructions->error = 3;
                    break;
                }
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

        audio_unregister_ringbuffer();

        xmp_end_player(ctx);
        xmp_release_module(ctx);
    }

    xmp_free_context(ctx);
    return 0;
}

void *audiothread_timidity(void *param)
{
    audiothread_instructions_t *instructions = (audiothread_instructions_t *)param;

    if (mid_init ("rom://timidity/timidity.cfg") < 0)
    {
        instructions->error = 1;
        return 0;
    }

    MidIStream *stream = mid_istream_open_file (instructions->filename);
    if (stream == NULL)
    {
        mid_exit();
        instructions->error = 2;
    }
    else
    {
        MidSongOptions options;
        options.rate = SAMPLERATE;
        options.format = MID_AUDIO_S16LSB;
        options.channels = 2;
        options.buffer_size = BUFSIZE / 4;

        MidSong *song = mid_song_load (stream, &options);
        mid_istream_close (stream);

        if (song == NULL)
        {
            instructions->error = 3;
        }
        else
        {
            uint32_t *buffer = malloc(BUFSIZE);

            uint32_t total_time = mid_song_get_total_time(song);
            char *title = mid_song_get_meta (song, MID_SONG_TEXT);

            ATOMIC(strcpy(instructions->modulename, title == NULL ? "no song title" : title));
            ATOMIC(strcpy(instructions->tracker, "midi"));

            mid_song_set_volume(song, 100);
            mid_song_start(song);

            audio_register_ringbuffer(AUDIO_FORMAT_16BIT, SAMPLERATE, BUFSIZE);

            int bytes_read;
            while (instructions->exit == 0 && (bytes_read = mid_song_read_wave(song, (void *)buffer, BUFSIZE)))
            {
                int numsamples = bytes_read / 4;
                uint32_t *samples = buffer;

                uint32_t current_time = mid_song_get_time(song);
                ATOMIC(sprintf(instructions->position, "%lu/%lu", current_time / 1000, total_time / 1000));
                while (numsamples > 0)
                {
                    int actual_written = audio_write_stereo_data(samples, numsamples);
                    if (actual_written < 0)
                    {
                        instructions->error = 3;
                        break;
                    }
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

            audio_unregister_ringbuffer();

            mid_song_free (song);

            free(buffer);
        }
    }

    mid_exit();
    return 0;
}

char lower(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return (c - 'A') + 'a';
    }

    return c;
}

audiothread_instructions_t * play(char *filename)
{
    audiothread_instructions_t *inst = malloc(sizeof(audiothread_instructions_t));
    memset(inst, 0, sizeof(audiothread_instructions_t));
    strcpy(inst->filename, filename);

    // Figure out the extension for this file.
    char ext[32] = { 0 };
    int extlen = 0;
    int fnamelen = strlen(filename);
    while (extlen < sizeof(ext) - 1)
    {
        int pos = fnamelen - (extlen + 1);
        if (pos < 0)
        {
            break;
        }

        if (filename[pos] == '.')
        {
            break;
        }

        ext[extlen++] = lower(filename[pos]);
        ext[extlen] = 0;
    }

    if (strcmp(ext, "dim") == 0)
    {
        inst->thread = thread_create("audio", &audiothread_timidity, inst);
    }
    else
    {
        inst->thread = thread_create("audio", &audiothread_xmp, inst);
    }
    thread_priority(inst->thread, 1);
    thread_start(inst->thread);
    return inst;
}

void stop(audiothread_instructions_t *inst)
{
    inst->exit = 1;
    thread_join(inst->thread);
    thread_destroy(inst->thread);
    free(inst);
}

typedef struct
{
    char filename[256];
    int type;
} file_t;

int list_comp(const void *a, const void *b)
{
    file_t *file_a = (file_t *)a;
    file_t *file_b = (file_t *)b;

    if (file_a->type == DT_DIR && file_b->type != DT_DIR)
    {
        return -1;
    }
    if (file_a->type != DT_DIR && file_b->type == DT_DIR)
    {
        return 1;
    }
    return strcmp(file_a->filename, file_b->filename);
}

file_t *list_files(const char *path, int *numentries)
{
    DIR *dir = opendir(path);

    int is_root = strcmp(path, "rom://") == 0;
    int count = 0;
    file_t *files = 0;
    while (1)
    {
        struct dirent* direntp = readdir(dir);
        if (direntp == 0)
        {
            break;
        }

        if (is_root && direntp->d_type == DT_DIR && strcmp(direntp->d_name, "timidity") == 0)
        {
            // Hide timidity directory for aesthetic reasons.
            continue;
        }
        if (is_root && direntp->d_type == DT_DIR && strcmp(direntp->d_name, "..") == 0)
        {
            // Hide up directory on root.
            continue;
        }
        if (direntp->d_type == DT_DIR && strcmp(direntp->d_name, ".") == 0)
        {
            // Hide current directory everywhere.
            continue;
        }

        count++;
        if (files == 0)
        {
            files = malloc(sizeof(file_t) * count);
        }
        else
        {
            files = realloc(files, sizeof(file_t) * count);
        }
        memset(&files[count - 1], 0, sizeof(file_t));
        strcpy(files[count - 1].filename, direntp->d_name);
        files[count - 1].type = direntp->d_type;
    }

    // Don't forget to close the directory!
    closedir(dir);

    *numentries = count;
    if (count > 0 && files)
    {
        qsort(files, count, sizeof(file_t), &list_comp);
    }
    return files;
}

#define REPEAT_INITIAL_DELAY 500000
#define REPEAT_SUBSEQUENT_DELAY 25000

unsigned int repeat(unsigned int cur_state, int *repeat_count)
{
    // A held button will "repeat" itself 40x a second after a 1/2 second hold delay.
    if (*repeat_count < 0)
    {
        // If we have never pushed this button, don't try repeating
        // if it happened to be held.
        return 0;
    }

    if (cur_state == 0)
    {
        // Button isn't held, no repeats.
        timer_stop(*repeat_count);
        *repeat_count = -1;
        return 0;
    }

    if (timer_left(*repeat_count) == 0)
    {
        // We should restart this timer with a shorter delay
        // because we're in a repeat zone.
        timer_stop(*repeat_count);
        *repeat_count = timer_start(REPEAT_SUBSEQUENT_DELAY);
        return 1;
    }

    // Not currently being repeated.
    return 0;
}

void repeat_init(unsigned int pushed_state, int *repeat_count)
{
    if (pushed_state == 0)
    {
        // Haven't pushed the button yet.
        return;
    }

    // Clear out old timer if needed.
    if (*repeat_count >= 0)
    {
        timer_stop(*repeat_count);
    }

    // Set up a half-second timer for our first repeat.
    *repeat_count = timer_start(REPEAT_INITIAL_DELAY);
}

void main()
{
    // Get settings so we know how many controls to read.
    eeprom_t settings;
    eeprom_read(&settings);

    // Initialize some crappy video.
    video_init(VIDEO_COLOR_1555);
    video_set_background_color(rgb(48, 48, 48));

    // Initialize the ROMFS.
    romfs_init_default();

    // Initialize audio system.
    audio_init();

    // Set up our root directory.
    char rootpath[1024];
    strcpy(rootpath, "rom://");

    int filecount = 0;
    file_t *files = list_files(rootpath, &filecount);

    audiothread_instructions_t *instructions = 0;

    // Calculate the size of the screen.
    int numlines = ((video_height() - 40) / 8) - 7;
    int cursor = 0;
    int top = 0;
    int repeats[4] = { -1, -1, -1, -1 };
    while ( 1 )
    {
        // Grab inputs.
        maple_poll_buttons();
        jvs_buttons_t pressed = maple_buttons_pressed();
        jvs_buttons_t held = maple_buttons_held();

        if (pressed.player1.up || (settings.system.players >= 2 && pressed.player2.up))
        {
            repeat_init(pressed.player1.up, &repeats[0]);
            repeat_init(pressed.player2.up, &repeats[1]);

            if (cursor > 0)
            {
                cursor--;
            }
            if (cursor < top)
            {
                top = cursor;
            }
        }
        else if (pressed.player1.down || (settings.system.players >= 2 && pressed.player2.down))
        {
            repeat_init(pressed.player1.down, &repeats[2]);
            repeat_init(pressed.player2.down, &repeats[3]);

            if (cursor < (filecount - 1))
            {
                cursor++;
            }
            if (cursor >= (top + numlines))
            {
                top = cursor - (numlines - 1);
            }
        }
        else if (repeat(held.player1.up, &repeats[0]) || (settings.system.players >= 2 && repeat(held.player2.up, &repeats[1])))
        {
            if (cursor > 0)
            {
                cursor--;
            }
            if (cursor < top)
            {
                top = cursor;
            }
        }
        else if (repeat(held.player1.down, &repeats[2]) || (settings.system.players >= 2 && repeat(held.player2.down, &repeats[3])))
        {
            if (cursor < (filecount - 1))
            {
                cursor++;
            }
            if (cursor >= (top + numlines))
            {
                top = cursor - (numlines - 1);
            }
        }

        if (pressed.player1.start || (settings.system.players >= 2 && pressed.player2.start))
        {
            if (files[cursor].type == DT_DIR)
            {
                // Enter directory.
                char filename[1024];
                strcpy(filename, rootpath);
                strcat(filename, "/");
                strcat(filename, files[cursor].filename);
                realpath(filename, rootpath);

                // List it.
                free(files);
                files = list_files(rootpath, &filecount);
                top = 0;
                cursor = 0;
            }
            else
            {
                // Play file.
                char filename[1024];
                strcpy(filename, rootpath);
                strcat(filename, "/");
                strcat(filename, files[cursor].filename);

                char *realname = realpath(filename, 0);

                if (instructions)
                {
                    stop(instructions);
                }
                if (realname)
                {
                    instructions = play(realname);
                    free(realname);
                }
            }
        }

        // Stop flickering by not having the instructions bits updated while we are
        // drawing them.
        uint32_t old_irq = irq_disable();

        if (instructions)
        {
            if (instructions->error)
            {
                // Display info about error.
                video_draw_debug_text(
                    20,
                    20,
                    rgb(255, 255, 255),
                    "Filename: %s\nName: %s\nTracker: %s\nPlayback Position: %s",
                    instructions->filename + 5,
                    "<<cannot play file>>",
                    "N/A",
                    "N/A"
                );
            }
            else
            {
                // Display info about playback.
                video_draw_debug_text(
                    20,
                    20,
                    rgb(255, 255, 255),
                    "Filename: %s\nName: %s\nTracker: %s\nPlayback Position: %s",
                    instructions->filename + 5,
                    instructions->modulename,
                    instructions->tracker,
                    instructions->position
                );
            }
        }
        else
        {
            // Display nothing.
            video_draw_debug_text(
                20,
                20,
                rgb(255, 255, 255),
                "Filename: %s\nName: %s\nTracker: %s\nPlayback Position: %s",
                "<<nothing>>",
                "N/A",
                "N/A",
                "N/A"
            );
        }

        irq_restore(old_irq);

        // Display current directory.
        video_draw_debug_text(20, 20 + (8 * 5), rgb(128, 255, 128), rootpath + 5);

        for (int i = 0; i < numlines; i++)
        {
            // Figure out the actual file.
            int fileoff = i + top;
            if (fileoff >= filecount) { break; }

            // Draw directories and files.
            if (files[fileoff].type == DT_DIR)
            {
                video_draw_debug_text(20, 20 + (8 * (7 + i)), rgb(128, 128, 255), "  [ %s ]", files[fileoff].filename);
            }
            else
            {
                video_draw_debug_text(20, 20 + (8 * (7 + i)), rgb(255, 255, 255), "  %s", files[fileoff].filename);
            }

            // Draw cursor.
            video_draw_debug_text(20, 20 + (8 * (7 + i)), rgb(255, 255, 255), "%c", fileoff == cursor ? '>' : ' ');
        }

        // Wait for vblank and draw it! We do this polling-style instead of interrupt style
        // because for some reason it flickers with the latter. Not sure why, but I suspect
        // that the audio thread priorities are wack. However, its not worth it to track down.
        old_irq = irq_disable();
        video_display_on_vblank();
        irq_restore(old_irq);
    }
}

void test()
{
    video_init(VIDEO_COLOR_1555);

    while ( 1 )
    {
        video_fill_screen(rgb(48, 48, 48));
        video_draw_debug_text(320 - 56, 236, rgb(255, 255, 255), "test mode stub");
        video_display_on_vblank();
    }
}
