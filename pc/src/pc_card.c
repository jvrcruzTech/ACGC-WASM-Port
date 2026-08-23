/* pc_card.c - memory card API.
 * Native desktop uses local file I/O.
 * Web builds keep open files as raw byte buffers and sync those bytes directly. */
#include "pc_platform.h"
#ifndef __EMSCRIPTEN__
#include <sys/stat.h>   /* mkdir (Linux), stat */
#ifdef _WIN32
#include <direct.h>  /* _mkdir */
#include <windows.h>
#else
#include <dirent.h>
#endif
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define CARD_RESULT_READY     0
#define CARD_RESULT_BUSY     -1
#define CARD_RESULT_WRONGDEVICE -2
#define CARD_RESULT_NOCARD   -3
#define CARD_RESULT_NOFILE   -4
#define CARD_RESULT_IOERROR  -5
#define CARD_RESULT_BROKEN   -6
#define CARD_RESULT_EXIST    -7
#define CARD_RESULT_NOENT    -8
#define CARD_RESULT_INSSPACE -9
#define CARD_RESULT_NOPERM   -10
#define CARD_RESULT_LIMIT    -11
#define CARD_RESULT_NAMETOOLONG -12
#define CARD_RESULT_ENCODING -13
#define CARD_RESULT_CANCELED -14
#define CARD_RESULT_FATAL_ERROR -128

/* must match card.h layout (20 bytes); extra state goes in side table */
typedef struct {
    s32   chan;
    s32   fileNo;
    s32   offset;
    s32   length;
    u16   iBlock;
} CARDFileInfo_PC;

#define CARD_MAX_OPEN 4
typedef struct {
    CARDFileInfo_PC* owner;   /* NULL = slot free */
#ifdef __EMSCRIPTEN__
    u8*              data;
    s32              length;
    s32              capacity;
#else
    FILE*            fp;
    char             path[512];
#endif
    char             filename[64];
} CARDOpenSlot;

static CARDOpenSlot card_slots[CARD_MAX_OPEN];

static CARDOpenSlot* card_slot_alloc(CARDFileInfo_PC* fi) {
    int i;
    for (i = 0; i < CARD_MAX_OPEN; i++) {
        if (card_slots[i].owner == NULL) {
            card_slots[i].owner = fi;
#ifdef __EMSCRIPTEN__
            card_slots[i].data = NULL;
            card_slots[i].length = 0;
            card_slots[i].capacity = 0;
#else
            card_slots[i].fp = NULL;
            card_slots[i].path[0] = '\0';
#endif
            card_slots[i].filename[0] = '\0';
            return &card_slots[i];
        }
    }
    return NULL;
}

static CARDOpenSlot* card_slot_find(CARDFileInfo_PC* fi) {
    int i;
    for (i = 0; i < CARD_MAX_OPEN; i++) {
        if (card_slots[i].owner == fi) return &card_slots[i];
    }
    return NULL;
}

static void card_slot_free(CARDFileInfo_PC* fi) {
    int i;
    for (i = 0; i < CARD_MAX_OPEN; i++) {
        if (card_slots[i].owner == fi) {
#ifdef __EMSCRIPTEN__
            if (card_slots[i].data) free(card_slots[i].data);
            card_slots[i].data = NULL;
            card_slots[i].length = 0;
            card_slots[i].capacity = 0;
#endif
            card_slots[i].owner = NULL;
#ifndef __EMSCRIPTEN__
            card_slots[i].fp = NULL;
#endif
            return;
        }
    }
}

/* Per-channel directory: chan 0 = card_a, chan 1 = card_b */
static const char* card_dir[2] = { "save/card_a", "save/card_b" };
static int card_mounted[2] = {0, 0};
static s32 card_async_result[2] = { CARD_RESULT_READY, CARD_RESULT_READY };
static int card_async_busy_ticks[2] = { 0, 0 };

static const char* get_card_dir(s32 chan) {
    if (chan >= 0 && chan <= 1) return card_dir[chan];
    return card_dir[0];
}

static int card_chan_valid(s32 chan) {
    return chan >= 0 && chan <= 1;
}

static void card_async_accept(s32 chan, s32 final_result) {
    if (!card_chan_valid(chan)) return;
    card_async_result[chan] = final_result;
    card_async_busy_ticks[chan] = 1;
}

static void card_async_clear(s32 chan) {
    if (!card_chan_valid(chan)) return;
    card_async_result[chan] = CARD_RESULT_READY;
    card_async_busy_ticks[chan] = 0;
}

/* reject path traversal */
static int card_filename_safe(const char* name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "..")) return 0;
    if (strchr(name, '/') || strchr(name, '\\')) return 0;
    return 1;
}

#ifdef __EMSCRIPTEN__
EM_JS(int, pc_web_card_fetch_js, (int chan, const char* filename_ptr, unsigned int dest_ptr, unsigned int capacity), {
    const slot = chan === 1 ? "b" : "a";
    const filename = UTF8ToString(filename_ptr);
    const gameId = Module.acGameId || "animal_crossing";
    if (chan === 1) {
        const loadTravelCard = (result) => {
            const bytes = result && result.bytes;
            if (!bytes || !bytes.length) return 0;
            if (bytes.length < 467008 || bytes[0] !== 0x47 || bytes[1] !== 0x41 || bytes[2] !== 0x46) {
                console.error("[Animal Crossing card] travel memory_card is not a usable Animal Crossing GCI");
                return 0;
            }
            if (bytes.length > capacity) {
                console.error("[Animal Crossing card] fetch travel memory_card too large: " + bytes.length + " > " + capacity);
                return -1;
            }
            Module.acTravelCardB = {
                bytes: bytes,
                save: result.save || null,
                token: result.token || ""
            };
            HEAPU8.set(bytes, dest_ptr);
            console.log("[Animal Crossing card] loaded travel memory_card for card " + slot + " (" + bytes.length + " bytes)");
            return bytes.length;
        };
        if (Module.acTravelCardB && Module.acTravelCardB.bytes) {
            return loadTravelCard(Module.acTravelCardB);
        }
        return 0;
    }
    const entry = (Module.acSaveFiles || {})["memory_card"];
    if (!entry || !entry.bytes) return 0;
    const bytes = entry.bytes;
    if (bytes.length > capacity) {
        console.error("[Animal Crossing card] fetch memory_card too large: " + bytes.length + " > " + capacity);
        return -1;
    }
    if (Module.acPersistSaveFile) Module.acPersistSaveFile("memory_card", bytes);
    HEAPU8.set(bytes, dest_ptr);
    console.log("[Animal Crossing card] loaded memory_card for " + gameId + " (" + bytes.length + " bytes)");
    return bytes.length;
});

EM_JS(int, pc_web_card_store_js, (int chan, const char* filename_ptr, unsigned int data_ptr, unsigned int length), {
    const slot = chan === 1 ? "b" : "a";
    const filename = UTF8ToString(filename_ptr);
    const gameId = Module.acGameId || "animal_crossing";
    const data = HEAPU8.slice(data_ptr, data_ptr + length);
    if (chan === 1) {
        const travel = Module.acTravelCardB || {};
        if (!travel.save || !travel.save.id || !travel.token) {
            console.error("[Animal Crossing card] refused unauthorized card " + slot + " write for " + filename);
            return 0;
        }
        Module.acTravelCardB = Object.assign({}, Module.acTravelCardB || {}, { bytes: data });
        if (!Module.acPersistTravelSaveFile || !Module.acPersistTravelSaveFile(filename, data)) {
            console.error("[Animal Crossing card] failed to persist authorized card " + slot + " write for " + filename);
            return 0;
        }
        console.log("[Animal Crossing card] stored authorized travel " + filename + " for card " + slot + " (" + length + " bytes)");
        return 1;
    }
    const files = Module.acSaveFiles || (Module.acSaveFiles = {});
    const existed = !!files["memory_card"]?.exists;
    files["memory_card"] = { bytes: data, exists: true };
    if (!Module.acPersistSaveFile) return 1;
    Module.acPersistSaveFile("memory_card", data, existed);
    console.log("[Animal Crossing card] stored memory_card for " + gameId + " (" + length + " bytes)");
    return 1;
});

EM_JS(int, pc_web_card_delete_js, (int chan, const char* filename_ptr), {
    const slot = chan === 1 ? "b" : "a";
    const filename = UTF8ToString(filename_ptr);
    const gameId = Module.acGameId || "animal_crossing";
    const data = new Uint8Array(0);
    if (chan === 1) {
        const travel = Module.acTravelCardB || {};
        if (!travel.save || !travel.save.id || !travel.token) {
            console.error("[Animal Crossing card] refused unauthorized card " + slot + " delete for " + filename);
            return 0;
        }
        Module.acTravelCardB = Object.assign({}, Module.acTravelCardB || {}, { bytes: data });
        if (!Module.acPersistTravelSaveFile || !Module.acPersistTravelSaveFile(filename, data)) {
            console.error("[Animal Crossing card] failed to persist authorized card " + slot + " delete for " + filename);
            return 0;
        }
        console.log("[Animal Crossing card] cleared authorized travel " + filename + " for card " + slot);
        return 1;
    }
    const files = Module.acSaveFiles || (Module.acSaveFiles = {});
    const existed = !!files["memory_card"]?.exists;
    files["memory_card"] = { bytes: data, exists: true };
    if (!Module.acPersistSaveFile) return 1;
    Module.acPersistSaveFile("memory_card", data, existed);
    console.log("[Animal Crossing card] cleared memory_card for " + gameId);
    return 1;
});

EM_JS(void, pc_web_card_release_travel_js, (void), {
    if (Module.acTravelCardB) {
        Module.acTravelCardB.bytes = null;
        Module.acTravelCardB = null;
    }
    if (Module.acTravelCardBPrompt) {
        Module.acTravelCardBPrompt.status = "idle";
    }
    const sockets = self.__ACGC_TRAVEL_SOCKETS__ || [];
    while (sockets.length) {
        const socket = sockets.pop();
        try {
            socket.close(1000, "travel-complete");
        } catch (err) {
            console.warn("[Animal Crossing card] failed to close travel socket", err);
        }
    }
    console.log("[Animal Crossing card] released travel memory_card for card b");
});

static int web_card_fetch_into(s32 chan, const char* filename, u8** out_data, s32* out_len, s32 min_capacity) {
    s32 capacity = min_capacity > 0 ? min_capacity : (1024 * 1024);
    u8* data;
    int len;
    if (!card_filename_safe(filename)) return 0;
    data = (u8*)malloc(capacity);
    if (!data) return 0;
    len = pc_web_card_fetch_js(chan, filename, (unsigned int)data, (unsigned int)capacity);
    if (len <= 0) {
        free(data);
        return 0;
    }
    *out_data = data;
    *out_len = len;
    return 1;
}

int pc_web_card_load_file(s32 chan, const char* filename, u8** out_data, s32* out_len) {
    return web_card_fetch_into(chan, filename, out_data, out_len, 0);
}

int pc_web_card_store_file(s32 chan, const char* filename, const u8* data, s32 len) {
    if (!card_filename_safe(filename) || !data || len < 0) return 0;
    return pc_web_card_store_js(chan, filename, (unsigned int)data, (unsigned int)len);
}

void pc_web_card_release_travel(void) {
    pc_web_card_release_travel_js();
}
#endif

#define CARD_SECTOR_SIZE 8192

static void ensure_dirs(void) {
#ifdef __EMSCRIPTEN__
    /* Web builds do not mount or create a save filesystem. */
#else
#ifdef _WIN32
    _mkdir("save");
    _mkdir("save/card_a");
    _mkdir("save/card_b");
#else
    mkdir("save", 0755);
    mkdir("save/card_a", 0755);
    mkdir("save/card_b", 0755);
#endif
#endif
}

void CARDInit(void) {
    ensure_dirs();
    card_async_clear(0);
    card_async_clear(1);
}

s32 CARDMount(s32 chan, void* workArea, void* detachCallback) {
    (void)workArea; (void)detachCallback;
    if (chan < 0 || chan > 1) return CARD_RESULT_NOCARD;
    card_mounted[chan] = 1;
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, void* detachCb, void* attachCb) {
    s32 result = CARDMount(chan, workArea, detachCb);
    if (result == CARD_RESULT_READY) card_async_accept(chan, result);
    if (attachCb) ((void (*)(s32, s32))attachCb)(chan, result);
    return result;
}

s32 CARDUnmount(s32 chan) {
    if (chan >= 0 && chan <= 1) card_mounted[chan] = 0;
    card_async_clear(chan);
    return CARD_RESULT_READY;
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo_PC* fileInfo) {
#ifndef __EMSCRIPTEN__
    char path[512];
#endif
    CARDOpenSlot* slot;
    if (!card_filename_safe(fileName)) return CARD_RESULT_NAMETOOLONG;
#ifndef __EMSCRIPTEN__
    snprintf(path, sizeof(path), "%s/%s", get_card_dir(chan), fileName);
#endif

    fileInfo->chan = chan;
    fileInfo->offset = 0;

    slot = card_slot_alloc(fileInfo);
    if (!slot) return CARD_RESULT_IOERROR;

    strncpy(slot->filename, fileName, sizeof(slot->filename) - 1);
    slot->filename[sizeof(slot->filename) - 1] = '\0';
#ifdef __EMSCRIPTEN__
    if (!web_card_fetch_into(chan, fileName, &slot->data, &slot->length, 0)) {
        card_slot_free(fileInfo);
        return CARD_RESULT_NOFILE;
    }
    slot->capacity = slot->length;
    fileInfo->length = slot->length;
    return CARD_RESULT_READY;
#else
    strncpy(slot->path, path, sizeof(slot->path) - 1);
    slot->path[sizeof(slot->path) - 1] = '\0';
    slot->fp = fopen(path, "r+b");
    if (!slot->fp) {
        card_slot_free(fileInfo);
        return CARD_RESULT_NOFILE;
    }
    fseek(slot->fp, 0, SEEK_END);
    fileInfo->length = (s32)ftell(slot->fp);
    fseek(slot->fp, 0, SEEK_SET);
    return CARD_RESULT_READY;
#endif
}

s32 CARDClose(CARDFileInfo_PC* fileInfo) {
#ifndef __EMSCRIPTEN__
    CARDOpenSlot* slot = card_slot_find(fileInfo);
    if (slot && slot->fp) {
        fclose(slot->fp);
        slot->fp = NULL;
    }
#endif
    card_slot_free(fileInfo);
    return CARD_RESULT_READY;
}

s32 CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo_PC* fileInfo) {
#ifndef __EMSCRIPTEN__
    char path[512];
#endif
    CARDOpenSlot* slot;
    if (!card_filename_safe(fileName)) return CARD_RESULT_NAMETOOLONG;
#ifndef __EMSCRIPTEN__
    snprintf(path, sizeof(path), "%s/%s", get_card_dir(chan), fileName);
#endif

    fileInfo->chan = chan;
    fileInfo->offset = 0;
    fileInfo->length = size;

    slot = card_slot_alloc(fileInfo);
    if (!slot) return CARD_RESULT_IOERROR;

    strncpy(slot->filename, fileName, sizeof(slot->filename) - 1);
    slot->filename[sizeof(slot->filename) - 1] = '\0';
#ifdef __EMSCRIPTEN__
    slot->data = (u8*)calloc(1, size);
    if (!slot->data) { card_slot_free(fileInfo); return CARD_RESULT_IOERROR; }
    slot->length = size;
    slot->capacity = size;
    if (!pc_web_card_store_js(chan, fileName, (unsigned int)slot->data, (unsigned int)slot->length)) {
        card_slot_free(fileInfo);
        return CARD_RESULT_IOERROR;
    }
    return CARD_RESULT_READY;
#else
    strncpy(slot->path, path, sizeof(slot->path) - 1);
    slot->path[sizeof(slot->path) - 1] = '\0';

    slot->fp = fopen(path, "w+b");
    if (!slot->fp) { card_slot_free(fileInfo); return CARD_RESULT_IOERROR; }

    {
        u8* zeros = (u8*)calloc(1, size);
        if (!zeros) { fclose(slot->fp); slot->fp = NULL; card_slot_free(fileInfo); return CARD_RESULT_IOERROR; }
        size_t written = fwrite(zeros, 1, size, slot->fp);
        free(zeros);
        if (written != size) { fclose(slot->fp); slot->fp = NULL; card_slot_free(fileInfo); return CARD_RESULT_IOERROR; }
        fseek(slot->fp, 0, SEEK_SET);
    }
#ifdef __EMSCRIPTEN__
    pc_web_card_store_js(chan, fileName, path);
#endif

    return CARD_RESULT_READY;
#endif
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size, void* fileInfo, void* callback) {
    s32 result = CARDCreate(chan, fileName, size, (CARDFileInfo_PC*)fileInfo);
    if (result == CARD_RESULT_READY) card_async_accept(chan, result);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

s32 CARDRead(CARDFileInfo_PC* fileInfo, void* buf, s32 length, s32 offset) {
    CARDOpenSlot* slot = card_slot_find(fileInfo);
#ifdef __EMSCRIPTEN__
    if (!slot || !slot->data) return CARD_RESULT_NOFILE;
    if (offset < 0 || length < 0 || offset + length > slot->length) return CARD_RESULT_IOERROR;
    memcpy(buf, slot->data + offset, length);
    return CARD_RESULT_READY;
#else
    if (!slot || !slot->fp) return CARD_RESULT_NOFILE;
    fseek(slot->fp, offset, SEEK_SET);
    if ((s32)fread(buf, 1, length, slot->fp) != length) return CARD_RESULT_IOERROR;
    return CARD_RESULT_READY;
#endif
}

s32 CARDReadAsync(void* fileInfo, void* buf, s32 length, s32 offset, void* callback) {
    CARDFileInfo_PC* info = (CARDFileInfo_PC*)fileInfo;
    s32 result = CARDRead(info, buf, length, offset);
    if (result == CARD_RESULT_READY) card_async_accept(info->chan, result);
    if (callback) ((void (*)(s32, s32))callback)(info->chan, result);
    return result;
}

s32 CARDWrite(CARDFileInfo_PC* fileInfo, const void* buf, s32 length, s32 offset) {
    CARDOpenSlot* slot = card_slot_find(fileInfo);
#ifdef __EMSCRIPTEN__
    s32 needed;
    if (!slot || !slot->data) return CARD_RESULT_NOFILE;
    if (offset < 0 || length < 0) return CARD_RESULT_IOERROR;
    needed = offset + length;
    if (needed > slot->capacity) {
        u8* grown = (u8*)realloc(slot->data, needed);
        if (!grown) return CARD_RESULT_IOERROR;
        if (needed > slot->length) memset(grown + slot->length, 0, needed - slot->length);
        slot->data = grown;
        slot->capacity = needed;
    }
    memcpy(slot->data + offset, buf, length);
    if (needed > slot->length) slot->length = needed;
    fileInfo->length = slot->length;
    if (!pc_web_card_store_js(fileInfo->chan, slot->filename, (unsigned int)slot->data, (unsigned int)slot->length)) {
        return CARD_RESULT_IOERROR;
    }
    return CARD_RESULT_READY;
#else
    if (!slot || !slot->fp) return CARD_RESULT_NOFILE;
    fseek(slot->fp, offset, SEEK_SET);
    if ((s32)fwrite(buf, 1, length, slot->fp) != length) return CARD_RESULT_IOERROR;
    fflush(slot->fp);
    return CARD_RESULT_READY;
#endif
}

s32 CARDWriteAsync(void* fileInfo, const void* buf, s32 length, s32 offset, void* callback) {
    CARDFileInfo_PC* info = (CARDFileInfo_PC*)fileInfo;
    s32 result = CARDWrite(info, buf, length, offset);
    if (result == CARD_RESULT_READY) card_async_accept(info->chan, result);
    if (callback) ((void (*)(s32, s32))callback)(info->chan, result);
    return result;
}

s32 CARDDelete(s32 chan, const char* fileName) {
#ifndef __EMSCRIPTEN__
    char path[512];
#endif
    if (!card_filename_safe(fileName)) return CARD_RESULT_NAMETOOLONG;
#ifndef __EMSCRIPTEN__
    snprintf(path, sizeof(path), "%s/%s", get_card_dir(chan), fileName);
    remove(path);
#else
    pc_web_card_delete_js(chan, fileName);
#endif
    return CARD_RESULT_READY;
}

s32 CARDDeleteAsync(s32 chan, const char* fileName, void* callback) {
    s32 result = CARDDelete(chan, fileName);
    if (result == CARD_RESULT_READY) card_async_accept(chan, result);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

s32 CARDGetResultCode(s32 chan) {
    if (!card_chan_valid(chan)) return CARD_RESULT_NOCARD;
    if (card_async_busy_ticks[chan] > 0) {
        card_async_busy_ticks[chan]--;
        return CARD_RESULT_BUSY;
    }
    return card_async_result[chan];
}
s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed) {
    if (byteNotUsed) *byteNotUsed = 1024 * 1024; /* 1 MB free */
    if (filesNotUsed) *filesNotUsed = 100;
    return CARD_RESULT_READY;
}

s32 CARDGetSectorSize(s32 chan, u32* size) {
    if (size) *size = CARD_SECTOR_SIZE;
    return CARD_RESULT_READY;
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    if (memSize) *memSize = 16 * 1024 * 1024; /* 16 MB */
    if (sectorSize) *sectorSize = CARD_SECTOR_SIZE;
    return CARD_RESULT_READY;
}

s32 CARDProbe(s32 chan) { return CARD_RESULT_READY; }

s32 CARDCheck(s32 chan) { return CARD_RESULT_READY; }
s32 CARDCheckAsync(s32 chan, void* callback) {
    card_async_accept(chan, CARD_RESULT_READY);
    if (callback) ((void (*)(s32, s32))callback)(chan, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

typedef struct {
    char fileName[32];
    u32  length;
    u32  time;
    u8   gameName[4];
    u8   company[2];
    u8   bannerFormat;
    u32  iconAddr;
    u16  iconFormat;
    u16  iconSpeed;
    u32  commentAddr;
    u32  offsetBanner;
    u32  offsetBannerTlut;
    u32  offsetIcon[8];
    u32  offsetIconTlut;
    u32  offsetData;
} CARDStat;

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    memset(stat, 0, sizeof(CARDStat));
    return CARD_RESULT_READY;
}

s32 CARDSetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    return CARD_RESULT_READY;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, void* stat, void* callback) {
    card_async_accept(chan, CARD_RESULT_READY);
    if (callback) ((void (*)(s32, s32))callback)(chan, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

s32 CARDRename(s32 chan, const char* oldName, const char* newName) {
#ifndef __EMSCRIPTEN__
    char oldPath[512], newPath[512];
#else
    u8* data = NULL;
    s32 len = 0;
#endif
    if (!card_filename_safe(oldName) || !card_filename_safe(newName)) return CARD_RESULT_NAMETOOLONG;
#ifndef __EMSCRIPTEN__
    snprintf(oldPath, sizeof(oldPath), "%s/%s", get_card_dir(chan), oldName);
    snprintf(newPath, sizeof(newPath), "%s/%s", get_card_dir(chan), newName);
    rename(oldPath, newPath);
#else
    if (web_card_fetch_into(chan, oldName, &data, &len, 0)) {
        pc_web_card_store_js(chan, newName, (unsigned int)data, (unsigned int)len);
        free(data);
    }
    pc_web_card_delete_js(chan, oldName);
#endif
    return CARD_RESULT_READY;
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName, void* callback) {
    s32 result = CARDRename(chan, oldName, newName);
    if (result == CARD_RESULT_READY) card_async_accept(chan, result);
    if (callback) ((void (*)(s32, s32))callback)(chan, result);
    return result;
}

s32 CARDFormat(s32 chan) { return CARD_RESULT_READY; }
s32 CARDFormatAsync(s32 chan, void* callback) {
    card_async_accept(chan, CARD_RESULT_READY);
    if (callback) ((void (*)(s32, s32))callback)(chan, CARD_RESULT_READY);
    return CARD_RESULT_READY;
}

/* Scan a card directory for the first valid AC GCI file.
 * Returns 1 and writes full path to out_path if found, 0 otherwise. */
int pc_card_scan_for_gci(s32 chan, char* out_path, int out_size) {
    const char* dir = get_card_dir(chan);

#ifdef __EMSCRIPTEN__
    {
        u8* data = NULL;
        s32 len = 0;
        if (web_card_fetch_into(chan, "DobutsunomoriP_MURA.gci", &data, &len, 0)) {
            int valid = len >= 4 && data[0] == 'G' && data[1] == 'A' && data[2] == 'F';
            free(data);
            if (valid) {
                snprintf(out_path, out_size, "%s/DobutsunomoriP_MURA.gci", dir);
                return 1;
            }
        }
    }
#else
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char search[300];
    snprintf(search, sizeof(search), "%s\\*.gci", dir);
    h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        /* Quick-validate: read first 4 bytes of GCI header (gameName) */
        {
            char full[512];
            FILE* fp;
            u8 hdr[4];
            snprintf(full, sizeof(full), "%s/%s", dir, fd.cFileName);
            fp = fopen(full, "rb");
            if (fp) {
                if (fread(hdr, 1, 4, fp) == 4 && hdr[0] == 'G' && hdr[1] == 'A' && hdr[2] == 'F') {
                    fclose(fp);
                    snprintf(out_path, out_size, "%s", full);
                    FindClose(h);
                    return 1;
                }
                fclose(fp);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    struct dirent* ent;
    if (!d) return 0;
    while ((ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        int len = strlen(name);
        if (len < 5) continue;
        if (strcasecmp(name + len - 4, ".gci") != 0) continue;
        {
            char full[512];
            FILE* fp;
            u8 hdr[4];
            snprintf(full, sizeof(full), "%s/%s", dir, name);
            fp = fopen(full, "rb");
            if (fp) {
                if (fread(hdr, 1, 4, fp) == 4 && hdr[0] == 'G' && hdr[1] == 'A' && hdr[2] == 'F') {
                    fclose(fp);
                    snprintf(out_path, out_size, "%s", full);
                    closedir(d);
                    return 1;
                }
                fclose(fp);
            }
        }
    }
    closedir(d);
#endif
#endif

    return 0;
}
