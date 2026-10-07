/* Read-only metadata reader for exFAT and unsigned contiguous PFS/PFSC.
 * Format references: PSBrew/MkPFS (GPL-3.0), commit
 * 1d5df56ae29390900e888fa12e5f9856aa797adc, pfs.py and exfat.py.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * No mounting, full-image extraction, decryption or writes are performed.
 */
#include "image_metadata.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

#define DIRECTORY_LIMIT (4U * 1024 * 1024)
#define ICON_LIMIT (4U * 1024 * 1024)
#define BACKGROUND_LIMIT (16U * 1024 * 1024)
#define PARAM_LIMIT 65536U
#define READ_LIMIT (64U * 1024 * 1024)
#define PFSC_BLOCK 65536U

typedef struct Budget Budget;
typedef struct View View;
struct Budget { const ImageSource *source; ImageMetadata *result; unsigned calls; time_t began; };
struct View { uint64_t size; void *context; int (*read)(View *, uint64_t, void *, size_t); Budget *budget; };
typedef struct { View *parent; uint64_t base; } Extent;
typedef struct {
    View *raw; uint64_t table, data, cached; bool valid;
    unsigned char decoded[PFSC_BLOCK];
    uint64_t previous; bool previous_valid;
    unsigned char previous_decoded[PFSC_BLOCK];
} Compressed;
typedef struct { View *view; uint32_t block; uint64_t count, inode_blocks; } Pfs;
typedef struct { uint64_t size, stored, base; uint32_t flags; uint16_t mode; } Inode;
typedef struct { uint32_t inode, type; char name[256]; } Dirent;
typedef struct { uint32_t cluster; uint64_t size; bool contiguous, directory; } ExEntry;
typedef struct { View *view; uint64_t fat, heap; uint32_t cluster_size, count, root; } Exfat;

static uint16_t u16(const unsigned char *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t u32(const unsigned char *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const unsigned char *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }
static bool fits(uint64_t offset, uint64_t length, uint64_t size) { return offset <= size && length <= size - offset; }
static int fail(View *v, const char *why) {
    if (!v->budget->result->status[0]) snprintf(v->budget->result->status, sizeof v->budget->result->status, "%s", why);
    return -1;
}
static int read_view(View *v, uint64_t offset, void *out, size_t length) {
    if (!fits(offset, length, v->size)) return fail(v, "Invalid image offsets");
    return v->read(v, offset, out, length);
}
static int read_source(View *v, uint64_t offset, void *out, size_t length) {
    Budget *b = v->budget;
    if (++b->calls > 8192 || length > READ_LIMIT - b->result->bytes_read || time(NULL) - b->began > 45)
        return fail(v, "Metadata read budget exceeded");
    b->result->bytes_read += length;
    return b->source->read_at(b->source->context, offset, out, length) ? fail(v, "Image read interrupted") : 0;
}
static int read_extent(View *v, uint64_t offset, void *out, size_t length) {
    Extent *e = v->context;
    if (!fits(e->base, offset, e->parent->size)) return fail(v, "Invalid PFS extent");
    return read_view(e->parent, e->base + offset, out, length);
}
static int read_compressed(View *v, uint64_t offset, void *buffer, size_t length) {
    Compressed *c = v->context; unsigned char *out = buffer;
    while (length) {
        uint64_t index = offset / PFSC_BLOCK;
        size_t within = (size_t)(offset % PFSC_BLOCK), take = PFSC_BLOCK - within;
        if (take > length) take = length;
        if (!c->valid || c->cached != index) {
            if (c->previous_valid && c->previous == index) {
                /* exFAT alternates file data and FAT lookups. Keep both blocks. */
                unsigned char swap[4096];
                for (size_t pos=0;pos<PFSC_BLOCK;pos+=sizeof swap) {
                    memcpy(swap,c->decoded+pos,sizeof swap);
                    memcpy(c->decoded+pos,c->previous_decoded+pos,sizeof swap);
                    memcpy(c->previous_decoded+pos,swap,sizeof swap);
                }
                c->previous=c->cached;c->cached=index;
            } else {
            if(c->valid){memcpy(c->previous_decoded,c->decoded,PFSC_BLOCK);c->previous=c->cached;c->previous_valid=true;}
            unsigned char pointers[16];
            if (read_view(c->raw, c->table + index * 8, pointers, sizeof pointers)) return -1;
            uint64_t begin = u64(pointers), end = u64(pointers + 8);
            if (begin < c->data || end <= begin || end - begin > PFSC_BLOCK || !fits(begin, end - begin, c->raw->size))
                return fail(v, "Invalid PFSC block table");
            size_t stored_size = (size_t)(end - begin);
            if (stored_size == PFSC_BLOCK) {
                if (read_view(c->raw, begin, c->decoded, stored_size)) return -1;
            } else {
                unsigned char *stored = malloc(stored_size);
                if (!stored) return fail(v, "Out of memory");
                int rc = read_view(c->raw, begin, stored, stored_size);
                uLongf decoded_size = PFSC_BLOCK;
                if (!rc && (uncompress(c->decoded, &decoded_size, stored, (uLong)stored_size) != Z_OK || decoded_size != PFSC_BLOCK))
                    rc = fail(v, "Invalid compressed metadata block");
                free(stored); if (rc) return -1;
            }
            c->cached = index; c->valid = true;
            }
        }
        memcpy(out, c->decoded + within, take); out += take; offset += take; length -= take;
    }
    return 0;
}
static int compressed_view(View *raw, View *logical, Compressed *state) {
    unsigned char h[48];
    if (read_view(raw, 0, h, sizeof h)) return -1;
    uint64_t size = u64(h + 40), table = u64(h + 24), data = u64(h + 32);
    if (u32(h) != 0x43534650 || u32(h + 4) || u32(h + 8) != 6 ||
        u32(h + 12) != PFSC_BLOCK || u64(h + 16) != PFSC_BLOCK || !size ||
        size > INT64_MAX || size % PFSC_BLOCK || table < sizeof h ||
        !fits(table, (size / PFSC_BLOCK + 1) * 8, raw->size) ||
        data < table + (size / PFSC_BLOCK + 1) * 8 || data > raw->size)
        return fail(raw, "Unsupported PFSC header");
    memset(state, 0, sizeof *state); state->raw = raw; state->table = table; state->data = data;
    *logical = (View){.size = size, .context = state, .read = read_compressed, .budget = raw->budget};
    return 0;
}
static int read_inode(Pfs *p, uint32_t number, Inode *inode) {
    unsigned char h[168]; uint64_t per = p->block / sizeof h;
    if (!per || number >= p->count || number / per >= p->inode_blocks) return fail(p->view, "Invalid inode number");
    uint64_t offset = (1 + number / per) * p->block + (number % per) * sizeof h;
    if (read_view(p->view, offset, h, sizeof h)) return -1;
    inode->mode = u16(h); inode->flags = u32(h + 4);
    bool compressed = (inode->flags & 1) != 0;
    inode->size = u64(h + (compressed ? 16 : 8));
    inode->stored = u64(h + (compressed ? 8 : 16));
    inode->base = (uint64_t)u32(h + 100) * p->block;
    if (inode->size > INT64_MAX || !fits(inode->base, inode->stored, p->view->size) ||
        (!(inode->flags & 1) && inode->size > inode->stored)) return fail(p->view, "Invalid inode extent");
    return 0;
}
static int inode_view(Pfs *p, const Inode *inode, View *raw, Extent *extent, View *logical, Compressed *compressed) {
    *extent = (Extent){.parent = p->view, .base = inode->base};
    *raw = (View){.size = inode->stored, .context = extent, .read = read_extent, .budget = p->view->budget};
    if (inode->flags & 1) {
        if (compressed_view(raw, logical, compressed)) return -1;
        if (inode->size > logical->size) return fail(raw, "Truncated PFSC logical stream");
        logical->size = inode->size;
    } else { *logical = *raw; logical->size = inode->size; }
    return 0;
}
/* Only names used for metadata discovery are needed; non-ASCII game filenames
 * are skipped rather than used as paths on the host. */
static int dirents(View *v, Dirent **entries, size_t *count) {
    if (!v->size || v->size > DIRECTORY_LIMIT) return fail(v, "PFS directory exceeds metadata limit");
    unsigned char *data = malloc((size_t)v->size);
    size_t capacity = (size_t)v->size / 16;
    if (capacity > 16384) capacity = 16384;
    Dirent *result = calloc(capacity, sizeof *result);
    if (!data || !result) { free(data); free(result); return fail(v, "Out of memory"); }
    int rc = read_view(v, 0, data, (size_t)v->size); size_t n = 0;
    for (size_t pos = 0; !rc && pos + 16 <= v->size;) {
        unsigned char *d = data + pos; uint32_t name = u32(d + 8), size = u32(d + 12);
        if (!u32(d) && !u32(d + 4) && !name && !size) break;
        if (size < 17 || size % 8 || name > size - 16 || size > v->size - pos || u32(d + 4) < 2 || u32(d + 4) > 5) {
            /* Some images retain stale bytes after the valid directory prefix.
             * Metadata-only discovery may use that prefix; never interpret the
             * invalid suffix or follow its offsets. Empty/invalid prefixes fail. */
            if (!n) rc = fail(v, "Invalid PFS directory");
            break;
        }
        if (name && name < sizeof result[n].name && !memchr(d + 16, 0, name)) {
            if (n == capacity) { rc = fail(v, "Too many PFS directory entries"); break; }
            result[n].inode = u32(d); result[n].type = u32(d + 4);
            memcpy(result[n].name, d + 16, name); result[n].name[name] = 0; n++;
        }
        pos += size;
    }
    free(data);
    if (rc) { free(result); return -1; }
    *entries = result; *count = n; return 0;
}
static int inode_entries(Pfs *p, uint32_t number, Dirent **entries, size_t *count) {
    Inode inode; View raw, logical; Extent extent;
    Compressed *compressed = calloc(1, sizeof *compressed);
    if (!compressed) return fail(p->view, "Out of memory");
    int rc = read_inode(p, number, &inode);
    if (!rc && !(inode.mode & 0x4000)) rc = fail(p->view, "Expected PFS directory");
    if (!rc) rc = inode_view(p, &inode, &raw, &extent, &logical, compressed);
    if (!rc) rc = dirents(&logical, entries, count);
    free(compressed); return rc;
}
static int small_file(View *v, unsigned char **out, size_t *length, size_t limit) {
    if (!v->size || v->size > limit) return 0;
    unsigned char *data = calloc(1, (size_t)v->size + 1);
    if (!data) return fail(v, "Out of memory");
    if (read_view(v, 0, data, (size_t)v->size)) { free(data); return -1; }
    free(*out); *out = data; *length = (size_t)v->size; return 0;
}
static int stream_background(View *v) {
    const ImageSource *source=v->budget->source;
    if(!source->background_write || v->size<148 || v->size>BACKGROUND_LIMIT)return fail(v,"Invalid DDS background size");
    unsigned char *chunk=malloc(65536);if(!chunk)return fail(v,"Background chunk allocation failed");
    int rc=0;
    for(uint64_t offset=0;offset<v->size;){
        size_t n=v->size-offset>65536?65536:(size_t)(v->size-offset);
        if(read_view(v,offset,chunk,n)||source->background_write(source->context,chunk,n)){rc=fail(v,"Background cache stream failed");break;}
        offset+=n;
    }
    free(chunk);
    if(!rc){v->budget->result->background_size=(size_t)v->size;v->budget->result->background_streamed=1;}
    return rc;
}
static int pfs_metadata_file(Pfs *p, uint32_t number, int kind) {
    Inode inode; View raw, logical; Extent extent;
    Compressed *compressed = calloc(1, sizeof *compressed);
    if (!compressed) return fail(p->view, "Out of memory");
    int rc = read_inode(p, number, &inode);
    if (!rc && !(inode.mode & 0x8000)) rc = fail(p->view, "Expected PFS file");
    if (!rc) rc = inode_view(p, &inode, &raw, &extent, &logical, compressed);
    ImageMetadata *m = p->view->budget->result;
    if (!rc && kind==3) rc=stream_background(&logical);
    else if (!rc) rc = small_file(&logical, kind == 2 ? &m->background : kind == 1 ? &m->icon : &m->param,
                           kind == 2 ? &m->background_size : kind == 1 ? &m->icon_size : &m->param_size, kind == 2 ? BACKGROUND_LIMIT : kind == 1 ? ICON_LIMIT : PARAM_LIMIT);
    free(compressed); return rc;
}
static int parse_image(View *v, unsigned depth);
static int parse_pfs(View *v, unsigned depth, const unsigned char *header) {
    uint64_t version = u64(header); uint16_t mode = u16(header + 28);
    if ((version != 1 && version != 2) || (mode & 7)) return fail(v, "Signed, encrypted or 64-bit PFS is not supported");
    Pfs p = {.view = v, .block = u32(header + 32), .count = u64(header + 48), .inode_blocks = u64(header + 64)};
    if (p.block < 512 || p.block > 1048576 || (p.block & (p.block - 1)) ||
        !p.count || p.count > 1000000 || !p.inode_blocks || p.inode_blocks >= v->size / p.block)
        return fail(v, "Invalid PFS geometry");
    Extent super_extent = {.parent = v, .base = (p.inode_blocks + 1) * p.block};
    View super = {.size = p.block, .context = &super_extent, .read = read_extent, .budget = v->budget};
    Dirent *entries = NULL; size_t count = 0; uint32_t root = UINT32_MAX;
    if (dirents(&super, &entries, &count)) return -1;
    for (size_t i = 0; i < count; i++) if (!strcmp(entries[i].name, "uroot")) root = entries[i].inode;
    free(entries); entries = NULL;
    if (root == UINT32_MAX) return fail(v, "PFS root not found");
    if (inode_entries(&p, root, &entries, &count)) return -1;
    uint32_t sce = UINT32_MAX, inner = UINT32_MAX; unsigned files = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].type == 3 && (!strcasecmp(entries[i].name,"fakelib") || !strcasecmp(entries[i].name,"fakelib2"))) v->budget->result->backport_files=1;
        if (entries[i].type == 3 && !strcasecmp(entries[i].name, "sce_sys")) sce = entries[i].inode;
        if (entries[i].type == 2) { inner = entries[i].inode; files++; }
    }
    free(entries); entries = NULL;
    if (sce != UINT32_MAX) {
        if (inode_entries(&p, sce, &entries, &count)) return -1;
        int rc = 0;
        for (size_t i = 0; i < count && !rc; i++) {
            if (entries[i].type != 2) continue;
            if (!strcasecmp(entries[i].name, "param.json")) rc = pfs_metadata_file(&p, entries[i].inode, false);
            if (!v->budget->source->skip_icon && !strcasecmp(entries[i].name, "icon0.png")) rc = pfs_metadata_file(&p, entries[i].inode, true);
        }
        const char *background=v->budget->source->background_write?"pic0.dds":"pic0.png";
        for (size_t i = 0; i < count; i++) if (!v->budget->source->skip_background && entries[i].type == 2 && !strcasecmp(entries[i].name, background)) { (void)pfs_metadata_file(&p, entries[i].inode, v->budget->source->background_write?3:2); break; }
        free(entries); return rc;
    }
    if (files != 1) return fail(v, "No metadata or single wrapped image in PFS");
    Inode inode; Extent extent; View raw, logical;
    Compressed *compressed = calloc(1, sizeof *compressed);
    if (!compressed) return fail(v, "Out of memory");
    int rc = read_inode(&p, inner, &inode);
    if (!rc) rc = inode_view(&p, &inode, &raw, &extent, &logical, compressed);
    if (!rc) rc = parse_image(&logical, depth + 1);
    free(compressed); return rc;
}
static bool valid_cluster(Exfat *fs, uint32_t cluster) { return cluster >= 2 && (uint64_t)cluster - 2 < fs->count; }
static uint64_t cluster_offset(Exfat *fs, uint32_t cluster) { return fs->heap + ((uint64_t)cluster - 2) * fs->cluster_size; }
static int next_cluster(Exfat *fs, uint32_t *cluster) {
    unsigned char value[4];
    if (!valid_cluster(fs, *cluster) || read_view(fs->view, fs->fat + (uint64_t)*cluster * 4, value, 4)) return -1;
    *cluster = u32(value); return 0;
}
static int exfat_data(Exfat *fs, const ExEntry *entry, bool directory, unsigned char **out, size_t *length, size_t limit) {
    if ((!directory && !entry->size) || entry->size > limit) return 0;
    uint32_t cluster = entry->cluster;
    size_t used = 0, capacity = directory && !entry->size ? limit : (size_t)entry->size;
    if (!capacity) return 0;
    unsigned char *data = calloc(1, capacity + 1);
    if (!data) return fail(fs->view, "Out of memory");
    int rc = 0;
    while (used < capacity) {
        if (!valid_cluster(fs, cluster)) { rc = fail(fs->view, "Invalid exFAT cluster chain"); break; }
        size_t take = capacity - used;
        if (take > fs->cluster_size) take = fs->cluster_size;
        if (read_view(fs->view, cluster_offset(fs, cluster), data + used, take)) { rc = -1; break; }
        if (directory) {
            for (size_t i = 0; i + 32 <= take; i += 32)
                if (!data[used + i]) { used += i; goto complete; }
        }
        used += take;
        if (used >= capacity) break;
        if (entry->contiguous) cluster++;
        else if (next_cluster(fs, &cluster)) { rc = -1; break; }
        if (directory && cluster >= 0xfffffff8) goto complete;
    }
    if (directory && !entry->size && used == capacity) rc = fail(fs->view, "exFAT directory exceeds metadata limit");
complete:
    if (rc) { free(data); return -1; }
    free(*out); *out = data; *length = used; return 0;
}
static int exfat_find(Exfat *fs, const ExEntry *directory, const char *target, ExEntry *found) {
    unsigned char *data = NULL; size_t size = 0;
    if (exfat_data(fs, directory, true, &data, &size, DIRECTORY_LIMIT)) return -1;
    int result = 0;
    for (size_t pos = 0; pos + 32 <= size;) {
        unsigned char *d = data + pos;
        if (d[0] != 0x85) { pos += 32; continue; }
        unsigned secondary = d[1]; size_t span = ((size_t)secondary + 1) * 32;
        if (secondary < 2 || span > size - pos) { result = fail(fs->view, "Invalid exFAT entry set"); break; }
        unsigned char *stream = d + 32; char name[256] = {0}; size_t chars = 0;
        if (stream[0] != 0xc0) { result = fail(fs->view, "Missing exFAT stream entry"); break; }
        for (unsigned j = 2; j <= secondary; j++) {
            unsigned char *part = d + j * 32;
            if (part[0] != 0xc1) continue;
            for (unsigned k = 0; k < 15 && chars < stream[3]; k++, chars++) {
                uint16_t ch = u16(part + 2 + 2*k);
                name[chars] = ch < 128 && ch ? (char)ch : '?';
            }
        }
        if (chars == stream[3] && !strcasecmp(name, target)) {
            *found = (ExEntry){.cluster = u32(stream + 20), .size = u64(stream + 24),
                              .contiguous = (stream[1] & 2) != 0, .directory = (u16(d + 4) & 16) != 0};
            if (u64(stream + 8) > found->size) result = fail(fs->view, "Invalid exFAT valid length");
            else result = 1;
            break;
        }
        pos += span;
    }
    free(data); return result;
}
static int parse_exfat(View *v, const unsigned char *header) {
    unsigned sector_shift = header[108], cluster_shift = header[109];
    if (u16(header + 510) != 0xaa55 || sector_shift < 9 || sector_shift > 12 ||
        cluster_shift > 20 - sector_shift || header[110] != 1)
        return fail(v, "Unsupported exFAT geometry");
    uint32_t sector = 1U << sector_shift;
    Exfat fs = {.view = v, .fat = (uint64_t)u32(header + 80) * sector,
                .heap = (uint64_t)u32(header + 88) * sector, .count = u32(header + 92),
                .root = u32(header + 96), .cluster_size = 1U << (sector_shift + cluster_shift)};
    uint64_t fat_size = (uint64_t)u32(header + 84) * sector;
    if (!fs.count || !valid_cluster(&fs, fs.root) || (uint64_t)(fs.count + 2ULL) * 4 > fat_size ||
        !fits(fs.fat, fat_size, v->size) || !fits(fs.heap, (uint64_t)fs.count * fs.cluster_size, v->size))
        return fail(v, "Invalid exFAT bounds");
    ExEntry root = {.cluster = fs.root, .directory = true}, sce, entry;
    int rc = exfat_find(&fs, &root, "sce_sys", &sce);
    if (rc <= 0 || !sce.directory) return rc < 0 ? -1 : fail(v, "sce_sys folder not found in exFAT");
    ImageMetadata *m = v->budget->result;
    ExEntry backport;
    if (exfat_find(&fs,&root,"fakelib",&backport)>0 && backport.directory) m->backport_files=1;
    if (!m->backport_files && exfat_find(&fs,&root,"fakelib2",&backport)>0 && backport.directory) m->backport_files=1;
    rc = exfat_find(&fs, &sce, "param.json", &entry);
    if (rc < 0) return -1;
    if (rc && !entry.directory && exfat_data(&fs, &entry, false, &m->param, &m->param_size, PARAM_LIMIT)) return -1;
    rc = v->budget->source->skip_icon ? 0 : exfat_find(&fs, &sce, "icon0.png", &entry);
    if (rc < 0) return -1;
    if (rc && !entry.directory && exfat_data(&fs, &entry, false, &m->icon, &m->icon_size, ICON_LIMIT)) return -1;
    if(v->budget->source->skip_background)return 0;
    if(v->budget->source->background_write){
        rc=exfat_find(&fs,&sce,"pic0.dds",&entry);
        if(rc>0 && !entry.directory && entry.size>=148 && entry.size<=BACKGROUND_LIMIT){
            unsigned char *chunk=malloc(65536);uint32_t cluster=entry.cluster;uint64_t remaining=entry.size;int failed=!chunk;
            while(!failed && remaining){
                if(!valid_cluster(&fs,cluster)){failed=1;break;}
                size_t in_cluster=remaining<fs.cluster_size?(size_t)remaining:fs.cluster_size;
                for(size_t offset=0;offset<in_cluster;){
                    size_t n=in_cluster-offset>65536?65536:in_cluster-offset;
                    if(read_view(v,cluster_offset(&fs,cluster)+offset,chunk,n)||v->budget->source->background_write(v->budget->source->context,chunk,n)){failed=1;break;}
                    offset+=n;
                }
                remaining-=in_cluster;
                if(remaining){if(entry.contiguous)cluster++;else if(next_cluster(&fs,&cluster)){failed=1;break;}}
            }
            free(chunk);
            if(failed)fail(v,"Background cache stream failed");else {m->background_size=(size_t)entry.size;m->background_streamed=1;}
        }
    }else{
        rc = exfat_find(&fs, &sce, "pic0.png", &entry);
        if (rc > 0 && !entry.directory) (void)exfat_data(&fs, &entry, false, &m->background, &m->background_size, BACKGROUND_LIMIT);
    }
    return 0;
}
static int parse_image(View *v, unsigned depth) {
    if (depth > 3) return fail(v, "Image nesting limit exceeded");
    unsigned char header[512];
    if (read_view(v, 0, header, sizeof header)) return -1;
    if (!memcmp(header + 3, "EXFAT   ", 8)) return parse_exfat(v, header);
    if (u64(header + 8) == 20130315) return parse_pfs(v, depth, header);
    return fail(v, "Unsupported inner image filesystem");
}
int image_metadata_read(const ImageSource *source, ImageMetadata *result) {
    memset(result, 0, sizeof *result);
    Budget budget = {.source = source, .result = result, .began = time(NULL)};
    View root = {.size = source->size, .read = read_source, .budget = &budget};
    int rc = parse_image(&root, 0);
    if (result->icon && (result->icon_size < 8 || memcmp(result->icon, "\x89PNG\r\n\x1a\n", 8))) {
        free(result->icon); result->icon = NULL; result->icon_size = 0;
    }
    if (!rc && !result->status[0]) snprintf(result->status, sizeof result->status, "%s", result->param || result->icon ? "Embedded metadata" : "No embedded metadata");
    return rc;
}
void image_metadata_free(ImageMetadata *result) {
    free(result->param); free(result->icon); free(result->background); result->param = result->icon = result->background = NULL;
}
