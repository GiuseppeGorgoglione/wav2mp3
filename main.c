/******************************************************************************
 *
 * wav2mp3 - Convert WAV files to MP3
 * 
 * Written by Giuseppe Gorgoglione (gorgoglione@gmail.com)
 *
 * Requirements:
 *
 * (1) application is called with pathname as argument, e.g.
 *     <applicationname> F:\MyWavCollection
 *     all WAV-files contained directly in that folder are to be encoded to MP3
 *
 * (2) use all available CPU cores for the encoding process in an efficient way
 *     by utilizing multi-threading
 *
 * (3) statically link to lame encoder library
 *
 * (4) application should be compilable and runnable on Windows and Linux
 *
 * (5) the resulting MP3 files are to be placed within the same directory as
 *     the source WAV files, the filename extension should be changed
 *     appropriately to .MP3
 *
 * (6) non-WAV files in the given folder shall be ignored
 *
 * (7) multithreading shall be implemented by means of using Posix Threads
 *     (there exist implementations for Windows)
 *
 * (8) the Boost library shall not be used
 *
 * (9) the LAME encoder should be used with reasonable standard settings
 *     (e.g. quality based encoding with quality level "good")
 *
 ******************************************************************************/

#include <assert.h>
#include <dirent.h>
#include <lame.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int const WAV_ID_RIFF = 0x52494646; /* "RIFF" */
static int const WAV_ID_WAVE = 0x57415645; /* "WAVE" */
static int const WAV_ID_FMT  = 0x666d7420; /* "fmt " */
static int const WAV_ID_DATA = 0x64617461; /* "data" */

static short const WAVE_FORMAT_PCM = 0x0001;

struct request_struct
{
    char in_path[PATH_MAX];
    char out_path[PATH_MAX];
    pthread_t tid;

    struct request_struct *next;
};
typedef struct request_struct request_t;

static request_t *requests = NULL;
static pthread_mutex_t requests_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned long active_threads = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;


static char *base_name(const char *path)
{
#if defined OS_UNIX
    const char separator = '/';
#elif defined OS_WINDOWS
    const char separator = '\\';
#endif

    char *name = strrchr(path, separator);

    return (name != NULL) ? name++ : NULL;
}


static unsigned long get_number_of_cores(void)
{
    unsigned long number_of_threads;

#if defined OS_UNIX
    number_of_threads = (unsigned)sysconf(_SC_NPROCESSORS_CONF);
#elif defined OS_WINDOWS
    const char* str = getenv("NUMBER_OF_PROCESSORS");
    number_of_threads = strtol(str, 0, 10);
#endif

    return number_of_threads;
}


static int read_16_bits_le(FILE * file)
{
    unsigned char bytes[2] = { 0, 0 };

    fread(bytes, 1, 2, file);

    return (bytes[1] << 8) | bytes[0];
}


static int read_32_bits_le(FILE * file)
{
    unsigned char bytes[4] = { 0, 0, 0, 0 };

    fread(bytes, 1, 4, file);

    return (bytes[3] << 24) | (bytes[2] << 16) | (bytes[1] << 8) | bytes[0];
}


static int read_32_bits_be(FILE * file)
{
    unsigned char bytes[4] = { 0, 0, 0, 0 };

    fread(bytes, 1, 4, file);

    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

typedef struct wav_info_struct
{
    int channels;
    int samplerate;
    int bits_per_sample;
    int data_chunk_size;

} wav_info_t;


static int parse_wav_header(FILE *file, const char* path, wav_info_t *wav_info)
{
    int wav_id_riff = read_32_bits_be(file);
    if (wav_id_riff != WAV_ID_RIFF)
    {
        printf("info: file %s is not a WAV, skipping...\n", path);
        return -1;
    }

    int chunk_size = read_32_bits_le(file);
    (void)chunk_size; /* unused */

    int wav_id_wave = read_32_bits_be(file);
    if (wav_id_wave != WAV_ID_WAVE)
    {
        printf("info: WAV file %s is malformed, skipping...\n", path);
        return -1;
    }

    int fmt_chunk_id = read_32_bits_be(file);
    if (fmt_chunk_id != WAV_ID_FMT)
    {
        printf("info: WAV file %s is malformed, skipping...\n", path);
        return -1;
    }

    int fmt_chunk_size = read_32_bits_le(file);
    if (fmt_chunk_size != 16)
    {
        printf("info: WAV file %s is not in PCM format. Unsupported, skipping...\n", path);
        return -1;
    }

    int fmt_chunk_format = read_16_bits_le(file);
    if (fmt_chunk_format != WAVE_FORMAT_PCM)
    {
        printf("info: WAV file %s is not in PCM format. Unsupported, skipping...\n", path);
        return -1;
    }

    wav_info->channels = read_16_bits_le(file);
    if (wav_info->channels < 1 || wav_info->channels > 2)
    {
        printf("info: WAV file %s has an unsupported number of channels, skipping...\n", path);
        return -1;
    }

    wav_info->samplerate = read_32_bits_le(file);

    int fmt_chunk_byterate = read_32_bits_le(file);
    (void)fmt_chunk_byterate; /* unused */

    int fmt_chunk_block_align = read_16_bits_le(file);
    (void)fmt_chunk_block_align; /* unused */

    wav_info->bits_per_sample = read_16_bits_le(file);
    if (wav_info->bits_per_sample != 16)
    {
        printf("info: WAV file %s has an unsupported number of bits per second, skipping...\n", path);
        return -1;
    }

    /* seek to 'data' chunk */
    int data_chunk_id;
    while (((data_chunk_id = read_32_bits_be(file)) != WAV_ID_DATA) && !feof(file))
    {
        fseek(file, -3, SEEK_CUR);
    }

    if (feof(file))
    {
        printf("info: no sample data found in WAV file %s, skipping...\n", path);
        return -1;
    }

    wav_info->data_chunk_size = read_32_bits_le(file);

    return 0;
}


static void delete_encoding_request(void)
{
    /*
     * remove this child thread from the encoding request list
     */
    pthread_mutex_lock(&requests_lock);
    request_t **iterator;
    for (iterator = &requests; *iterator; )
    {
        request_t *entry = *iterator;
        if (entry->tid == pthread_self())
        {
            *iterator = entry->next;
            free(entry);
        }
        else
        {
            iterator = &entry->next;
        }
    }
    pthread_mutex_unlock(&requests_lock);

    /*
     * signal to the parent thread that this child thread is no longer active
     */
    pthread_mutex_lock(&mutex);
    active_threads--;
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
}


static void *encode_wav_to_mp3(void *params)
{
    request_t *request = (request_t *)params;
    const char* in_path = request->in_path;
    const char* out_path = request->out_path;

    lame_t  lame = lame_init();
    if (lame == NULL)
    {
        printf("error: Unable to init LAME encoder\n");
        delete_encoding_request();
        return NULL;
    }

    FILE *in_file = fopen(in_path, "rb");
    if (in_file == NULL)
    {
        printf("error: File '%s' not found\n", in_path);
        lame_close(lame);
        delete_encoding_request();
        return NULL;
    }

    wav_info_t wav_info;
    if (parse_wav_header(in_file, in_path, &wav_info) < 0)
    {
        fclose(in_file);
        lame_close(lame);
        delete_encoding_request();
        return NULL;
    }

    size_t bytes_per_sample = wav_info.bits_per_sample / 8;
    size_t samples = wav_info.data_chunk_size / bytes_per_sample;
    size_t samples_per_channel = samples / wav_info.channels;

    assert(bytes_per_sample == sizeof(short));

    lame_set_num_channels(lame, wav_info.channels);
    lame_set_in_samplerate(lame, wav_info.samplerate);
    lame_set_num_samples(lame, wav_info.data_chunk_size / (wav_info.channels * ((wav_info.bits_per_sample + 7) / 8)));

    unsigned char *pcm_buffer = (unsigned char *)malloc(wav_info.data_chunk_size);
    unsigned char *mp3_buffer = (unsigned char *)malloc(wav_info.data_chunk_size);
    short *channel_l = calloc(samples_per_channel, sizeof(short));
    short *channel_r = calloc(samples_per_channel, sizeof(short));

    int ret = lame_init_params(lame);

    if (pcm_buffer == NULL || mp3_buffer == NULL || channel_l == NULL || channel_r == NULL || ret < 0)
    {
        printf("error: unable to initialize LAME encoder and allocate required memory\n");
        if (pcm_buffer) free(pcm_buffer);
        if (mp3_buffer) free(mp3_buffer);
        if (channel_l) free(channel_l);
        if (channel_r) free(channel_r);
        fclose(in_file);
        lame_close(lame);
        delete_encoding_request();
        return NULL;
    }

    size_t samples_read = fread(pcm_buffer, bytes_per_sample, samples, in_file);
    if (samples_read < samples)
    {
        printf("info: WAV file %s is corrupted, skipping...\n", in_path);
        if (pcm_buffer) free(pcm_buffer);
        if (mp3_buffer) free(mp3_buffer);
        if (channel_l) free(channel_l);
        if (channel_r) free(channel_r);
        fclose(in_file);
        lame_close(lame);
        delete_encoding_request();
        return NULL;
    }

    if (wav_info.channels == 1)
    {
        memcpy(channel_l, pcm_buffer, bytes_per_sample * samples);
    }
    else if (wav_info.channels == 2)
    {
        for (int i = 0, j = 0; i < samples_per_channel; i++, j += 4)
        {
            channel_l[i] = (pcm_buffer[j + 1] << 8) | pcm_buffer[j + 0];
            channel_r[i] = (pcm_buffer[j + 3] << 8) | pcm_buffer[j + 2];
        }
    }

    printf("info: now encoding %s  -->  %s\n", in_path, out_path);

    int bytes_encoded = lame_encode_buffer(lame, channel_l, channel_r, samples_per_channel, mp3_buffer, wav_info.data_chunk_size);
    if (bytes_encoded < 0)
    {
        printf("error: encoding error for WAV file %s, skipping...\n", in_path);
        if (pcm_buffer) free(pcm_buffer);
        if (mp3_buffer) free(mp3_buffer);
        if (channel_l) free(channel_l);
        if (channel_r) free(channel_r);
        fclose(in_file);
        lame_close(lame);
        delete_encoding_request();
        return NULL;
    }

    FILE *out_file = fopen(out_path, "w+b");
    if (out_file == NULL)
    {
        printf("error: File '%s' not found\n", in_path);
        if (pcm_buffer) free(pcm_buffer);
        if (mp3_buffer) free(mp3_buffer);
        if (channel_l) free(channel_l);
        if (channel_r) free(channel_r);
        fclose(in_file);
        lame_close(lame);
        delete_encoding_request();
        return NULL;
    }

    fwrite(mp3_buffer, 1, bytes_encoded, out_file);

    bytes_encoded = lame_encode_flush(lame, mp3_buffer, wav_info.data_chunk_size);

    fwrite(mp3_buffer, 1, bytes_encoded, out_file);

    free(pcm_buffer);
    free(mp3_buffer);
    free(channel_l);
    free(channel_r);

    fclose(in_file);
    fclose(out_file);

    lame_close(lame);

    delete_encoding_request();

    return NULL;
}


void enforce_thread_limit_to_number_of_cores(void)
{
    pthread_mutex_lock(&mutex);

    while(active_threads >= get_number_of_cores())
    {
        pthread_cond_wait(&cond, &mutex);
    }

    active_threads++;

    pthread_mutex_unlock(&mutex);
}

void create_encoding_request(const char* in_path, const char* out_path, pthread_attr_t attr)
{
    /*
     * enforce the on-going encoding threads to be no more than the available cores
     */
    enforce_thread_limit_to_number_of_cores();

    /*
     * store the encoding request in a linked list
     */
    request_t *request = (request_t *)malloc(sizeof(request_t));

    strcpy(request->in_path, in_path);
    strcpy(request->out_path, out_path);
    request->tid = 0;

    pthread_mutex_lock(&requests_lock);
    request->next = requests;
    requests = request;
    pthread_mutex_unlock(&requests_lock);

    /*
     * execute the encoding request
     */
    pthread_create(&request->tid, &attr, encode_wav_to_mp3, request);
}


int main(int argc, const char **argv)
{
    if (argc < 2)
    {
        printf("%s: missing operand\nTry '%s --help' for more information.\n", base_name(argv[0]), base_name(argv[0]));
        return 0;
    }

    if (!strcmp(argv[1], "--help"))
    {
        printf("Usage: %s DIRECTORY\n", base_name(argv[0]));
        printf("Convert WAV files into MP3 files.\n\n");
        printf("Mandatory arguments:\n");
        printf("  DIRECTORY  All WAV files present in this folder will be converted to MP3\n\n");
        return 0;
    }

    DIR *directory = opendir(argv[1]);
    if (!directory)
    {
        printf("%s: directory '%s' not found.\n\n", base_name(argv[0]), argv[1]);
        return 0;
    }

    printf("Encoding WAVs to MP3s on %ld threads...\n", get_number_of_cores());

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    /* parent thread does not want to wait for the child threads to terminate */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    struct dirent *dir;
    while ((dir = readdir(directory)) != NULL)
    {
        /* skip current folder and parent folder */
        if (!(strcmp(dir->d_name, ".") && strcmp(dir->d_name, "..")))
        {
            continue;
        }

        /* build the complete input file name (path + basename + original ext) */
        char in_path[PATH_MAX];
        strcpy(in_path, argv[1]);
        strcat(in_path, "/");
        strcat(in_path, dir->d_name);

        char out_path[PATH_MAX];
        strcpy(out_path, in_path);
        char *ext = strstr( out_path, ".wav" );
        if (ext != NULL)
        {
            /* build the complete input file name (path + basename + ".mp3" ext) */
            strcpy(ext, ".mp3");
        }
        else
        {
            /* skip input files which do not have ".wav" ext */
            continue;
        }

        create_encoding_request(in_path, out_path, attr);
    }

    /* wait for the last on-going encoding requests */
    pthread_mutex_lock(&mutex);

    while(active_threads > 0)
    {
        pthread_cond_wait(&cond, &mutex);
    }

    pthread_mutex_unlock(&mutex);

    pthread_attr_destroy(&attr);

    return 0;
}
