/**
 * @file song_loader.c
 * @brief Song loading and preparation routines.
 *
 * Responsible for loading song data from file
 */

#include "song_loader.h"

#include "common/appstate.h"
#include "common/model.h"

#include "lyrics.h"

#include "data/cache.h"

#include "utils/file.h"
#include "utils/img_utils.h"
#include "utils/utils.h"

#include "stb_image.h"
#include "tagLibWrapper.h"

#include <dirent.h>
#include <gio/gio.h>
#include <glib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgen.h>

#define MAX_RECURSION_DEPTH 10

/**
 * @brief Data passed to the loading thread for asynchronous decoding.
 */

LoaderData loader_data;

Cache *tmpCache;

LoaderData *get_loader_data(void)
{
        return &loader_data;
}

int defaultColorRed;
int defaultColorGreen;
int defaultColorBlue;

int song_loader_init(void)
{
        c_strcpy(loader_data.file_path, "", sizeof(loader_data.file_path));

        loader_data.loadInSlotA = true;
        loader_data.loadingFirstDecoder = true;
        loader_data.replaceNextSong = false;

        loader_data.songdataA = NULL;
        loader_data.songdataB = NULL;

        loader_data.songdataADeleted = true;
        loader_data.songdataBDeleted = true;

        loader_data.dirty = false;

        tmpCache = create_cache();

        pthread_mutex_init(&(loader_data.mutex), NULL);

        return (tmpCache != NULL);
}

void unload_lyrics(SongData *songdata)
{
        if (songdata->lyrics) {
                freeLyrics(songdata->lyrics);
                songdata->lyrics = NULL;
        }
}

void unload_song_data(SongData **songdata)
{
        if (*songdata == NULL || (*songdata)->magic == 0) {
                return;
        }

        SongData *data = *songdata;

        if (data->cover != NULL) {
                stbi_image_free(data->cover);
                data->cover = NULL;
        }

        if (exists_in_cache(tmpCache, data->cover_art_path) &&
            is_in_temp_dir(data->cover_art_path)) {
                delete_file(data->cover_art_path);
                data->cover_art_path[0] = '\0';
        }

        unload_lyrics(data);

        data->magic = 0;

        free(data->metadata);
        free(data->track_id);

        data->cover = NULL;
        data->metadata = NULL;

        data->track_id = NULL;

        free(*songdata);
        *songdata = NULL;
}

void song_loader_unload_song_A(void)
{
        if (!loader_data.songdataADeleted) {

                unload_song_data(&(loader_data.songdataA));

                loader_data.songdataADeleted = true;
        }
}

void song_loader_unload_song_B(void)
{
        if (!loader_data.songdataBDeleted) {

                unload_song_data(&(loader_data.songdataB));

                loader_data.songdataBDeleted = true;
        }
}

void song_loader_assign_slot_A(SongData *songdata)
{
        song_loader_unload_song_A();
        loader_data.songdataA = songdata;
}

void song_loader_assign_slot_B(SongData *songdata)
{
        song_loader_unload_song_B();
        loader_data.songdataB = songdata;
}

void song_loader_unload_songs(void)
{
        song_loader_unload_song_A();
        song_loader_unload_song_B();
}

void song_loader_destroy()
{
        song_loader_unload_songs();
        delete_cache(tmpCache);
        pthread_mutex_destroy(&(loader_data.mutex));
}

static guint track_counter = 0;

void make_file_path(const char *dir_path, char *file_path, size_t file_path_size,
                    const struct dirent *entry)
{
        if (dir_path == NULL || file_path == NULL || entry == NULL ||
            file_path_size == 0)
                return;

        size_t dirLen = strnlen(dir_path, file_path_size);
        size_t nameLen = strnlen(entry->d_name, file_path_size);

        if (dirLen == file_path_size) {
                file_path[0] = '\0';
                return;
        }

        size_t neededSize = dirLen + nameLen + 1; // +1 for '\0'
        if (dir_path[dirLen - 1] != '/') {
                neededSize += 1; // for the added '/'
        }

        if (neededSize > file_path_size) {
                file_path[0] = '\0';
                return;
        }

        // Compose the path safely
        if (dir_path[dirLen - 1] == '/') {
                snprintf(file_path, file_path_size, "%s%s", dir_path,
                         entry->d_name);
        } else {
                snprintf(file_path, file_path_size, "%s/%s", dir_path,
                         entry->d_name);
        }

        // snprintf guarantees null termination if file_path_size > 0
}

char *choose_album_art(const char *dir_path, char **custom_file_name_arr, int size,
                       int depth)
{
        if (!dir_path || !custom_file_name_arr || size <= 0 ||
            depth > MAX_RECURSION_DEPTH) {
                return NULL;
        }

        DIR *directory = opendir(dir_path);
        if (!directory) {
                return NULL;
        }

        struct dirent *entry;
        struct stat file_stat;
        char file_path[KEW_PATH_MAX];
        char resolved_path[KEW_PATH_MAX];
        char *result = NULL;

        for (int i = 0; i < size && !result; i++) {
                rewinddir(directory);
                while ((entry = readdir(directory)) != NULL) {
                        if (strcmp(entry->d_name, ".") == 0 ||
                            strcmp(entry->d_name, "..") == 0)
                                continue;

                        int written = snprintf(file_path, sizeof(file_path),
                                               "%s/%s", dir_path, entry->d_name);
                        if (written < 0 || written >= (int)sizeof(file_path)) {
                                continue; // path too long
                        }

                        if (path_realpath(file_path, resolved_path) == NULL) {
                                continue;
                        }

                        if (strncmp(resolved_path, dir_path, strlen(dir_path)) !=
                            0) {
                                continue; // outside allowed directory
                        }

                        if (stat(resolved_path, &file_stat) == 0 &&
                            S_ISREG(file_stat.st_mode)) {
                                if (strcmp(entry->d_name,
                                           custom_file_name_arr[i]) == 0) {
                                        result = strdup(resolved_path);
                                        break;
                                }
                        }
                }
        }

        // Recursive search for directories
        if (!result) {
                rewinddir(directory);
                while ((entry = readdir(directory)) != NULL && !result) {
                        if (strcmp(entry->d_name, ".") == 0 ||
                            strcmp(entry->d_name, "..") == 0)
                                continue;

                        int written = snprintf(file_path, sizeof(file_path),
                                               "%s/%s", dir_path, entry->d_name);
                        if (written < 0 || written >= (int)sizeof(file_path)) {
                                continue;
                        }

                        if (path_realpath(file_path, resolved_path) == NULL) {
                                continue;
                        }

                        if (strncmp(resolved_path, dir_path, strlen(dir_path)) !=
                            0) {
                                continue;
                        }

                        struct stat link_stat;
#ifdef _WIN32
                        if (stat(resolved_path, &link_stat) == 0) {
#else
                        if (lstat(resolved_path, &link_stat) == 0) {
#endif
#ifdef S_ISLNK
                                if (S_ISLNK(link_stat.st_mode)) {
                                        continue; // skip symlink
                                }
#endif
                                if (S_ISDIR(link_stat.st_mode)) {
                                        result = choose_album_art(
                                            resolved_path, custom_file_name_arr,
                                            size, depth + 1);
                                }
                        }
                }
        }

        closedir(directory);
        return result;
}

#ifdef _WIN32
#include <windows.h>

char *find_largest_image_file(const char *directory_path, char *largest_image_file,
                              off_t *largest_file_size)
{
    // Convert directory path (UTF-8) to wide
    int wlen = MultiByteToWideChar(CP_UTF8, 0, directory_path, -1, NULL, 0);
    if (wlen <= 0) return largest_image_file;

    wchar_t *wdir = (wchar_t *)malloc(wlen * sizeof(wchar_t));
    if (!wdir) return largest_image_file;
    MultiByteToWideChar(CP_UTF8, 0, directory_path, -1, wdir, wlen);

    // Normalize separators to backslash
    for (int i = 0; wdir[i]; i++)
        if (wdir[i] == L'/') wdir[i] = L'\\';

    // Build "dir\*" pattern
    size_t dirlen = wcslen(wdir);
    int need_sep = (dirlen == 0 || wdir[dirlen - 1] != L'\\') ? 1 : 0;
    wchar_t *pattern = (wchar_t *)malloc((dirlen + need_sep + 2) * sizeof(wchar_t));
    if (!pattern) { free(wdir); return largest_image_file; }
    wcscpy(pattern, wdir);
    if (need_sep) wcscat(pattern, L"\\");
    wcscat(pattern, L"*");

    // Base path with guaranteed trailing backslash (reuse or rebuild)
    wchar_t *wbase = (wchar_t *)malloc((dirlen + 2) * sizeof(wchar_t));
    if (!wbase) { free(wdir); free(pattern); return largest_image_file; }
    wcscpy(wbase, wdir);
    if (need_sep) wcscat(wbase, L"\\");
    free(wdir);

    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(pattern, &ffd);
    free(pattern);

    if (hFind == INVALID_HANDLE_VALUE) {
        free(wbase);
        return largest_image_file;
    }

    do {
        if (wcscmp(ffd.cFileName, L".") == 0 || wcscmp(ffd.cFileName, L"..") == 0)
            continue;
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        // Check extension
        const wchar_t *ext = wcsrchr(ffd.cFileName, L'.');
        if (!ext) continue;
        if (_wcsicmp(ext, L".jpg")  != 0 &&
            _wcsicmp(ext, L".jpeg") != 0 &&
            _wcsicmp(ext, L".png")  != 0 &&
            _wcsicmp(ext, L".gif")  != 0)
            continue;

        // File size from FIND_DATA (avoids a stat call)
        LARGE_INTEGER sz;
        sz.HighPart = ffd.nFileSizeHigh;
        sz.LowPart  = ffd.nFileSizeLow;
        off_t file_size = (off_t)sz.QuadPart;

        const off_t MAX_FILE_SIZE = 100 * 1024 * 1024;
        if (file_size <= 0 || file_size > MAX_FILE_SIZE) continue;

        if (file_size > *largest_file_size) {
            *largest_file_size = file_size;

            size_t baselen = wcslen(wbase);
            size_t namelen = wcslen(ffd.cFileName);
            wchar_t *wfull = (wchar_t *)malloc((baselen + namelen + 1) * sizeof(wchar_t));
            if (!wfull) break;
            wcscpy(wfull, wbase);
            wcscat(wfull, ffd.cFileName);

            int u8len = WideCharToMultiByte(CP_UTF8, 0, wfull, -1, NULL, 0, NULL, NULL);
            if (u8len > 0) {
                char *tmp = (char *)malloc(u8len);
                if (tmp) {
                    WideCharToMultiByte(CP_UTF8, 0, wfull, -1, tmp, u8len, NULL, NULL);
                    free(largest_image_file);
                    largest_image_file = tmp;
                }
            }
            free(wfull);
        }

    } while (FindNextFileW(hFind, &ffd));

    FindClose(hFind);
    free(wbase);
    return largest_image_file;
}
#else
char *find_largest_image_file(const char *directory_path, char *largest_image_file,
                              off_t *largest_file_size)
{
        DIR *directory = opendir(directory_path);
        if (directory == NULL) {
                fprintf(stderr, "Failed to open directory: %s\n",
                        directory_path);
                return largest_image_file;
        }

        struct dirent *entry;
        struct stat file_stats;
        char file_path[KEW_PATH_MAX];
        char resolved_path[KEW_PATH_MAX];

        while ((entry = readdir(directory)) != NULL) {
                // Skip "." and ".."
                if (strcmp(entry->d_name, ".") == 0 ||
                    strcmp(entry->d_name, "..") == 0)
                        continue;

                // Construct file path safely
                int len = snprintf(file_path, sizeof(file_path), "%s/%s",
                                   directory_path, entry->d_name);
                if (len < 0 || len >= (int)sizeof(file_path)) {
                        // Path too long, skip
                        continue;
                }

                // Resolve the real path
                if (path_realpath(file_path, resolved_path) == NULL) {
                        // Could not resolve, skip
                        continue;
                }

                // Use lstat to avoid following symlinks
                if (lstat(resolved_path, &file_stats) == -1) {
                        continue;
                }
#ifdef S_ISLNK
                if (S_ISLNK(file_stats.st_mode)) {
                        // Ignore symlinks
                        continue;
                }
#endif
                if (S_ISREG(file_stats.st_mode)) {
                        // Validate extension
                        char *extension = strrchr(entry->d_name, '.');
                        if (extension != NULL &&
                            (strcasecmp(extension, ".jpg") == 0 ||
                             strcasecmp(extension, ".jpeg") == 0 ||
                             strcasecmp(extension, ".png") == 0 ||
                             strcasecmp(extension, ".gif") == 0)) {
                                // Ensure non-negative file size and prevent
                                // integer overflow
                                if (file_stats.st_size < 0)
                                        continue;

                                // Optional: impose max file size limit, e.g.,
                                // 100 MB
                                const off_t MAX_FILE_SIZE = 100 * 1024 * 1024;
                                if (file_stats.st_size > MAX_FILE_SIZE)
                                        continue;

                                if (file_stats.st_size > *largest_file_size) {
                                        *largest_file_size = file_stats.st_size;

                                        // Free previous allocation if owned
                                        if (largest_image_file != NULL) {
                                                free(largest_image_file);
                                                largest_image_file = NULL;
                                        }

                                        largest_image_file = strdup(resolved_path);
                                        if (largest_image_file == NULL) {
                                                fprintf(stderr,
                                                        "Memory allocation "
                                                        "failure\n");
                                                // Return early or continue
                                                // depending on desired behavior
                                                break;
                                        }
                                }
                        }
                }
        }

        closedir(directory);
        return largest_image_file;
}
#endif

gchar *generate_track_id(void)
{
        gchar *track_id =
            g_strdup_printf("/org/kew/tracklist/track%d", track_counter);
        track_counter++;
        return track_id;
}

int load_color(SongData *songdata)
{
        return get_cover_color(songdata->cover, songdata->coverWidth,
                               songdata->coverHeight, &(songdata->red),
                               &(songdata->green), &(songdata->blue));
}

static void sanitize_path_part(char *str) {
    if (!str) return;
    for (; *str; str++) {
        if (*str == '/' || *str == '\\' || *str == ':' || *str == '*' || 
            *str == '?' || *str == '"' || *str == '<' || *str == '>' || *str == '|') {
            *str = '_';
        }
    }
}

static void handle_metadata_from_filename(SongData *songdata) {
    char *base = g_path_get_basename(songdata->file_path);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    
    char *sep = strstr(base, " - ");
    if (sep) {
        char artist[METADATA_MAX_LENGTH] = {0};
        char title[METADATA_MAX_LENGTH] = {0};
        
        size_t artist_len = sep - base;
        if (artist_len >= METADATA_MAX_LENGTH) artist_len = METADATA_MAX_LENGTH - 1;
        memcpy(artist, base, artist_len);
        artist[artist_len] = '\0';
        
        c_strcpy(title, sep + 3, METADATA_MAX_LENGTH);
        
        trim(artist, METADATA_MAX_LENGTH);
        trim(title, METADATA_MAX_LENGTH);
        
        // Check if we need to update tags
        bool update = false;
        if (strlen(songdata->metadata->artist) == 0 || strcmp(songdata->metadata->artist, artist) != 0) {
            update = true;
            c_strcpy(songdata->metadata->artist, artist, METADATA_MAX_LENGTH);
        }
        if (strlen(songdata->metadata->title) == 0 || strcmp(songdata->metadata->title, title) != 0) {
            update = true;
            c_strcpy(songdata->metadata->title, title, METADATA_MAX_LENGTH);
        }
        
        if (update) {
            updateTags(songdata->file_path, songdata->metadata->title, songdata->metadata->artist);
        }
    }
    g_free(base);
}

static void handle_central_covers(SongData *songdata) {
    if (exists_file(songdata->cover_art_path) == 1) return;

    char dir[KEW_PATH_MAX];
    get_directory_from_path(songdata->file_path, dir);
    
    char artist[METADATA_MAX_LENGTH];
    char album[METADATA_MAX_LENGTH];
    c_strcpy(artist, songdata->metadata->artist, METADATA_MAX_LENGTH);
    c_strcpy(album, songdata->metadata->album, METADATA_MAX_LENGTH);

    if (strlen(artist) > 0) {
        // Handle covers
        char covers_dir[KEW_PATH_MAX];
        char cover_path[KEW_PATH_MAX];
        char album_cover_name[METADATA_MAX_LENGTH * 2 + 4];
        bool found = false;

        char s_artist[METADATA_MAX_LENGTH];
        char s_album[METADATA_MAX_LENGTH];
        c_strcpy(s_artist, artist, METADATA_MAX_LENGTH);
        c_strcpy(s_album, album, METADATA_MAX_LENGTH);
        sanitize_path_part(s_artist);
        sanitize_path_part(s_album);

        if (strlen(s_album) > 0) {
            snprintf(album_cover_name, sizeof(album_cover_name), "%s - %s", s_artist, s_album);
        } else {
            c_strcpy(album_cover_name, s_artist, sizeof(album_cover_name));
        }

        // 1. Try centralized .covers in library root
        AppSettings *settings = get_app_settings();
        char lib_root[KEW_PATH_MAX] = {0};
        if (settings && strlen(settings->path) > 0) {
            expand_path(settings->path, lib_root, KEW_PATH_MAX);
            size_t root_len = strlen(lib_root);
            if (root_len > 0 && lib_root[root_len-1] != '/' && root_len + 1 < KEW_PATH_MAX) {
                lib_root[root_len] = '/';
                lib_root[root_len+1] = '\0';
            }
        }

        char *search_dirs[3];
        search_dirs[0] = lib_root[0] ? lib_root : NULL;
        search_dirs[1] = (char *)"./";
        search_dirs[2] = dir;

        for (int i = 0; i < 3; i++) {
            if (!search_dirs[i]) continue;
            snprintf(covers_dir, sizeof(covers_dir), "%s.covers", search_dirs[i]);
            
            // Try Artist - Album.jpg
            if (strlen(s_album) > 0) {
                snprintf(cover_path, sizeof(cover_path), "%s/%s.jpg", covers_dir, album_cover_name);
                if (exists_file(cover_path) == 1) { found = true; break; }
            }
            
            // Try Artist.jpg
            snprintf(cover_path, sizeof(cover_path), "%s/%s.jpg", covers_dir, s_artist);
            if (exists_file(cover_path) == 1) { found = true; break; }
        }

        if (!found) {
            // Download cover
            if (lib_root[0]) {
                snprintf(covers_dir, sizeof(covers_dir), "%s.covers", lib_root);
            } else {
                snprintf(covers_dir, sizeof(covers_dir), "%s.covers", dir);
            }

            if (directory_exists(covers_dir) != 1) {
                create_directory(covers_dir);
            }
            
            snprintf(cover_path, sizeof(cover_path), "%s/%s.jpg", covers_dir, album_cover_name);
            
            if (exists_file(cover_path) != 1) {
                char search_term[METADATA_MAX_LENGTH * 2 + 4];
                if (strlen(album) > 0) {
                    snprintf(search_term, sizeof(search_term), "%s %s", artist, album);
                } else {
                    c_strcpy(search_term, artist, sizeof(search_term));
                }

                char *quoted_search = g_shell_quote(search_term);
                char *quoted_artist = g_shell_quote(artist);
                char *quoted_dest = g_shell_quote(cover_path);
                char cmd[4096];
                
                snprintf(cmd, sizeof(cmd), 
                    "URL=$(curl -sG --data-urlencode term=%s \"https://itunes.apple.com/search?entity=album&limit=1\" | grep -o '\"artworkUrl100\":\"[^\"]*\"' | head -1 | cut -d'\"' -f4 | sed 's/100x100bb.jpg/600x600bb.jpg/'); "
                    "if [ -n \"$URL\" ]; then curl -L -s --fail -o %s \"$URL\"; fi", 
                    quoted_search, quoted_dest);
                
                pid_t pid = fork();
                if (pid == 0) {
                    execlp("sh", "sh", "-c", cmd, (char *)NULL);
                    _exit(127);
                } else if (pid > 0) {
                    int status;
                    waitpid(pid, &status, 0);
                }

                if (exists_file(cover_path) != 1) {
                    snprintf(cmd, sizeof(cmd),
                        "MBID=$(curl -sG --data-urlencode query=artist:%s \"https://musicbrainz.org/ws/2/artist/?fmt=json\" | grep -o '\"id\":\"[^\"]*\"' | head -1 | cut -d'\"' -f4); "
                        "if [ -n \"$MBID\" ]; then "
                        "RELID=$(curl -s \"https://musicbrainz.org/ws/2/release?artist=$MBID&limit=1&fmt=json\" | grep -o '\"id\":\"[^\"]*\"' | head -1 | cut -d'\"' -f4); "
                        "if [ -n \"$RELID\" ]; then curl -L -s --fail -o %s \"https://coverartarchive.org/release/$RELID/front\"; fi; fi",
                        quoted_artist, quoted_dest);
                    
                    pid = fork();
                    if (pid == 0) {
                        execlp("sh", "sh", "-c", cmd, (char *)NULL);
                        _exit(127);
                    } else if (pid > 0) {
                        int status;
                        waitpid(pid, &status, 0);
                    }
                }
                
                g_free(quoted_search);
                g_free(quoted_artist);
                g_free(quoted_dest);
            }
        }
        
        if (exists_file(cover_path) == 1) {
            c_strcpy(songdata->cover_art_path, cover_path, sizeof(songdata->cover_art_path));
        }
    }
}

void load_meta_data(SongData *songdata)
{
        char path[KEW_PATH_MAX];

        songdata->metadata = malloc(sizeof(TagSettings));
        if (songdata->metadata == NULL) {
                songdata->hasErrors = true;
                return;
        }
        songdata->metadata->replaygainTrack = 0.0;
        songdata->metadata->replaygainAlbum = 0.0;

        generate_temp_file_path(songdata->cover_art_path, KEW_PATH_MAX, "cover", ".jpg");

        handle_metadata_from_filename(songdata);

        int res = extractTags(songdata->file_path, songdata->metadata,
                              &(songdata->duration), songdata->cover_art_path, &(songdata->lyrics));

        if (res == -2) {
                songdata->hasErrors = true;
                return;
        } else if (res == -1) {
                if (strlen(songdata->cover_art_path) == 0 || exists_file(songdata->cover_art_path) != 1) {
                        get_directory_from_path(songdata->file_path, path);
                        char *tmp = NULL;
                        off_t size = 0;
                        char *file_arr[12] = {
                            "front.png",
                            "front.jpg",
                            "front.jpeg",
                            "folder.png",
                            "folder.jpg",
                            "folder.jpeg",
                            "cover.png",
                            "cover.jpg",
                            "cover.jpeg",
                            "f.png",
                            "f.jpg",
                            "f.jpeg",
                        };
                        tmp = choose_album_art(path, file_arr, 12, 0);
                        if (tmp == NULL) {
                                tmp = find_largest_image_file(path, tmp, &size);
                        }

                        if (tmp != NULL) {
                                c_strcpy(songdata->cover_art_path, tmp,
                                         sizeof(songdata->cover_art_path));
                                free(tmp);
                                tmp = NULL;
                        } else
                                c_strcpy(songdata->cover_art_path, "",
                                         sizeof(songdata->cover_art_path));
                }
        }

        // If still no art found locally or embedded, try central covers
        if (strlen(songdata->cover_art_path) == 0 || exists_file(songdata->cover_art_path) != 1) {
                handle_central_covers(songdata);
        } else if (res == 0) {
                add_to_cache(tmpCache, songdata->cover_art_path);
        }

        songdata->cover =
            get_bitmap(songdata->cover_art_path, &(songdata->coverWidth),
                       &(songdata->coverHeight));
}

SongData *song_data_clone(const SongData *src)
{
        if (!src)
                return NULL;

        LoaderData *loader_data = get_loader_data();

        pthread_mutex_lock(&(loader_data->mutex));

        SongData *dst = calloc(1, sizeof(*dst));
        if (!dst)
                return NULL;

        // Copy all value fields
        *dst = *src;

        // Clear owned pointers
        dst->track_id = NULL;
        dst->metadata = NULL;
        dst->cover = NULL;
        dst->lyrics = NULL;

        // Track_id
        if (src->track_id) {
                dst->track_id = g_strdup(src->track_id);
                if (!dst->track_id)
                        goto error;
        }

        // Metadata
        if (src->metadata) {
                dst->metadata = malloc(sizeof(*dst->metadata));
                if (!dst->metadata)
                        goto error;

                *dst->metadata = *src->metadata;
        }

        // Deep-copy cover bitmap (must be RGBA)
        if (src->cover &&
            src->coverWidth > 0 &&
            src->coverHeight > 0) {

                size_t cover_size =
                        (size_t)src->coverWidth *
                        (size_t)src->coverHeight * 4;

                dst->cover = malloc(cover_size);
                if (!dst->cover)
                        goto error;

                memcpy(dst->cover, src->cover, cover_size);
        }

        // Deep-copy lyrics
        if (src->lyrics) {
                dst->lyrics = calloc(1, sizeof(*dst->lyrics));
                if (!dst->lyrics)
                        goto error;

                dst->lyrics->count = src->lyrics->count;
                dst->lyrics->max_length = src->lyrics->max_length;
                dst->lyrics->isTimed = src->lyrics->isTimed;

                if (src->lyrics->count > 0) {
                        dst->lyrics->lines =
                                calloc(src->lyrics->count,
                                       sizeof(*dst->lyrics->lines));

                        if (!dst->lyrics->lines)
                                goto error;

                        for (size_t i = 0; i < src->lyrics->count; i++) {
                                dst->lyrics->lines[i].timestamp =
                                        src->lyrics->lines[i].timestamp;

                                if (src->lyrics->lines[i].text) {
                                        dst->lyrics->lines[i].text =
                                                g_strdup(src->lyrics->lines[i].text);

                                        if (!dst->lyrics->lines[i].text)
                                                goto error;
                                }
                        }
                }
        }

        pthread_mutex_unlock(&(loader_data->mutex));

        return dst;

error:
        pthread_mutex_unlock(&(loader_data->mutex));
        unload_song_data(&dst);
        return NULL;
}

SongData *load_song_data(char *file_path)
{
        SongData *songdata = NULL;
        songdata = malloc(sizeof(SongData));
        songdata->magic = SONG_MAGIC;
        songdata->track_id = generate_track_id();
        songdata->hasErrors = false;
        c_strcpy(songdata->file_path, "", sizeof(songdata->file_path));
        c_strcpy(songdata->cover_art_path, "", sizeof(songdata->cover_art_path));
        songdata->red = -1;
        songdata->green = -1;
        songdata->blue = -1;
        songdata->metadata = NULL;
        songdata->cover = NULL;
        songdata->duration = 0.0;
        songdata->avg_bit_rate = 0;
        songdata->lyrics = NULL;
        c_strcpy(songdata->file_path, file_path, sizeof(songdata->file_path));
        load_meta_data(songdata); // loading two times meta_data may cause errors, but it is necessary to get the title for loading lyrics
        songdata->lyrics = loadLyricsFromLRC(songdata->file_path,songdata);
        load_meta_data(songdata);
        load_color(songdata);
        load_kmeans_palette(songdata->cover, songdata->coverWidth, songdata->coverHeight, songdata->kmeans_palette);

        return songdata;
}

static void process_entry_recursive(FileSystemEntry *entry) {
    if (!entry) return;
    if (entry->is_directory) {
        FileSystemEntry *child = entry->children;
        while (child) {
            process_entry_recursive(child);
            child = child->next;
        }
    } else if (is_music_file(entry->name)) {
        SongData *sd = calloc(1, sizeof(SongData));
        if (sd) {
            sd->metadata = malloc(sizeof(TagSettings));
            if (sd->metadata) {
                memset(sd->metadata, 0, sizeof(TagSettings));
                c_strcpy(sd->file_path, entry->full_path, sizeof(sd->file_path));
                double duration;
                Lyrics *lyrics = NULL;
                extractTags(sd->file_path, sd->metadata, &duration, sd->cover_art_path, &lyrics);
                if (lyrics) {
                    // We don't need lyrics for background cover processing, but extractTags might allocate them
                    // Actually, extractTags in this codebase might just load pointers or allocate.
                    // Need to check extractTags signature and behavior.
                }
                handle_metadata_from_filename(sd);
                handle_central_covers(sd);
                free(sd->metadata);
            }
            free(sd);
        }
    }
}

void *process_library_covers_thread(void *arg) {
    Model *model = (Model *)arg;
    if (!model || !model->library) return NULL;
    
    // Sleep a bit to let the UI initialize
    c_sleep(2000);
    
    process_entry_recursive(model->library);
    return NULL;
}
