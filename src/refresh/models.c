/*
Copyright (C) 1997-2001 Id Software, Inc.
Copyright (C) 2008 Andrey Nazarov

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "gl.h"
#include "format/md2.h"
#if USE_MD3
#include "format/md3.h"
#endif
#include "format/sp2.h"

#define MOD_Malloc(size)    Hunk_TryAlloc(&model->hunk, size)

#define CHECK(x)    if (!(x)) { ret = Q_ERR(ENOMEM); goto fail; }

#if MAX_ALIAS_VERTS > TESS_MAX_VERTICES
#error TESS_MAX_VERTICES
#endif

#if MD2_MAX_TRIANGLES > TESS_MAX_INDICES / 3
#error TESS_MAX_INDICES
#endif

// during registration it is possible to have more models than could actually
// be referenced during gameplay, because we don't want to free anything until
// we are sure we won't need it.
#define MAX_RMODELS     (MAX_MODELS * 2)

static model_t      r_models[MAX_RMODELS];
static int          r_numModels;

static model_t *MOD_Alloc(void)
{
    model_t *model;
    int i;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            break;
        }
    }

    if (i == r_numModels) {
        if (r_numModels == MAX_RMODELS) {
            return NULL;
        }
        r_numModels++;
    }

    return model;
}

static model_t *MOD_Find(const char *name)
{
    model_t *model;
    int i;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            continue;
        }
        if (!FS_pathcmp(model->name, name)) {
            return model;
        }
    }

    return NULL;
}

static void MOD_List_f(void)
{
    static const char types[4] = "FASE";
    int     i, count;
    model_t *model;
    size_t  bytes;

    Com_Printf("------------------\n");
    bytes = count = 0;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            continue;
        }
        Com_Printf("%c %8zu : %s\n", types[model->type],
                   model->hunk.mapped, model->name);
        bytes += model->hunk.mapped;
        count++;
    }
    Com_Printf("Total models: %d (out of %d slots)\n", count, r_numModels);
    Com_Printf("Total resident: %zu\n", bytes);
}

void MOD_FreeUnused(void)
{
    model_t *model;
    int i;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            continue;
        }
        if (model->registration_sequence == registration_sequence) {
            // make sure it is paged in
            Com_PageInMemory(model->hunk.base, model->hunk.cursize);
        } else {
            // don't need this model
            Hunk_Free(&model->hunk);
            memset(model, 0, sizeof(*model));
        }
    }
}

void MOD_FreeAll(void)
{
    model_t *model;
    int i;

    for (i = 0, model = r_models; i < r_numModels; i++, model++) {
        if (!model->type) {
            continue;
        }

        Hunk_Free(&model->hunk);
        memset(model, 0, sizeof(*model));
    }

    r_numModels = 0;
}

static void LittleBlock(void *out, const void *in, size_t size)
{
    memcpy(out, in, size);
#if USE_BIG_ENDIAN
    for (int i = 0; i < size / 4; i++)
        ((uint32_t *)out)[i] = LittleLong(((uint32_t *)out)[i]);
#endif
}

static int MOD_LoadSP2(model_t *model, const void *rawdata, size_t length)
{
    dsp2header_t header;
    dsp2frame_t *src_frame;
    mspriteframe_t *dst_frame;
    char buffer[SP2_MAX_FRAMENAME];
    int i;

    if (length < sizeof(header))
        return Q_ERR_FILE_TOO_SMALL;

    // byte swap the header
    LittleBlock(&header, rawdata, sizeof(header));

    if (header.ident != SP2_IDENT)
        return Q_ERR_UNKNOWN_FORMAT;
    if (header.version != SP2_VERSION)
        return Q_ERR_UNKNOWN_FORMAT;
    if (header.numframes < 1) {
        // empty models draw nothing
        model->type = MOD_EMPTY;
        return Q_ERR_SUCCESS;
    }
    if (header.numframes > SP2_MAX_FRAMES)
        return Q_ERR_TOO_MANY;
    if (sizeof(dsp2header_t) + sizeof(dsp2frame_t) * header.numframes > length)
        return Q_ERR_BAD_EXTENT;

    Hunk_Begin(&model->hunk, sizeof(mspriteframe_t) * header.numframes);
    model->type = MOD_SPRITE;

    model->spriteframes = MOD_Malloc(sizeof(mspriteframe_t) * header.numframes);
    model->numframes = header.numframes;

    src_frame = (dsp2frame_t *)((byte *)rawdata + sizeof(dsp2header_t));
    dst_frame = model->spriteframes;
    for (i = 0; i < header.numframes; i++) {
        dst_frame->width = (int32_t)LittleLong(src_frame->width);
        dst_frame->height = (int32_t)LittleLong(src_frame->height);

        dst_frame->origin_x = (int32_t)LittleLong(src_frame->origin_x);
        dst_frame->origin_y = (int32_t)LittleLong(src_frame->origin_y);

        if (!Q_memccpy(buffer, src_frame->name, 0, sizeof(buffer))) {
            Com_WPrintf("%s has bad frame name\n", model->name);
            dst_frame->image = R_NOTEXTURE;
        } else {
            FS_NormalizePath(buffer);
            dst_frame->image = IMG_Find(buffer, IT_SPRITE, IF_NONE);
        }

        src_frame++;
        dst_frame++;
    }

    Hunk_End(&model->hunk);

    return Q_ERR_SUCCESS;
}

static int MOD_ValidateMD2(dmd2header_t *header, size_t length)
{
    size_t end;

    // check ident and version
    if (header->ident != MD2_IDENT)
        return Q_ERR_UNKNOWN_FORMAT;
    if (header->version != MD2_VERSION)
        return Q_ERR_UNKNOWN_FORMAT;

    // check triangles
    if (header->num_tris < 1)
        return Q_ERR_TOO_FEW;
    if (header->num_tris > MD2_MAX_TRIANGLES)
        return Q_ERR_TOO_MANY;

    end = header->ofs_tris + sizeof(dmd2triangle_t) * header->num_tris;
    if (header->ofs_tris < sizeof(*header) || end < header->ofs_tris || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header->ofs_tris % q_alignof(dmd2triangle_t))
        return Q_ERR_BAD_ALIGN;

    // check st
    if (header->num_st < 3)
        return Q_ERR_TOO_FEW;
    if (header->num_st > MAX_ALIAS_VERTS)
        return Q_ERR_TOO_MANY;

    end = header->ofs_st + sizeof(dmd2stvert_t) * header->num_st;
    if (header->ofs_st < sizeof(*header) || end < header->ofs_st || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header->ofs_st % q_alignof(dmd2stvert_t))
        return Q_ERR_BAD_ALIGN;

    // check xyz and frames
    if (header->num_xyz < 3)
        return Q_ERR_TOO_FEW;
    if (header->num_xyz > MAX_ALIAS_VERTS)
        return Q_ERR_TOO_MANY;
    if (header->num_frames < 1)
        return Q_ERR_TOO_FEW;
    if (header->num_frames > MD2_MAX_FRAMES)
        return Q_ERR_TOO_MANY;

    end = sizeof(dmd2frame_t) + (header->num_xyz - 1) * sizeof(dmd2trivertx_t);
    if (header->framesize < end || header->framesize > MD2_MAX_FRAMESIZE)
        return Q_ERR_BAD_EXTENT;
    if (header->framesize % q_alignof(dmd2frame_t))
        return Q_ERR_BAD_ALIGN;

    end = header->ofs_frames + (size_t)header->framesize * header->num_frames;
    if (header->ofs_frames < sizeof(*header) || end < header->ofs_frames || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header->ofs_frames % q_alignof(dmd2frame_t))
        return Q_ERR_BAD_ALIGN;

    // check skins
    if (header->num_skins) {
        if (header->num_skins > MAX_ALIAS_SKINS)
            return Q_ERR_TOO_MANY;

        end = header->ofs_skins + (size_t)MD2_MAX_SKINNAME * header->num_skins;
        if (header->ofs_skins < sizeof(*header) || end < header->ofs_skins || end > length)
            return Q_ERR_BAD_EXTENT;
    }

    if (header->skinwidth < 1 || header->skinwidth > MD2_MAX_SKINWIDTH)
        return Q_ERR_INVALID_FORMAT;
    if (header->skinheight < 1 || header->skinheight > MD2_MAX_SKINHEIGHT)
        return Q_ERR_INVALID_FORMAT;

    return Q_ERR_SUCCESS;
}

static int MOD_LoadMD2(model_t *model, const void *rawdata, size_t length)
{
    dmd2header_t    header;
    dmd2frame_t     *src_frame;
    dmd2trivertx_t  *src_vert;
    dmd2triangle_t  *src_tri;
    dmd2stvert_t    *src_tc;
    char            *src_skin;
    maliasframe_t   *dst_frame;
    maliasvert_t    *dst_vert;
    maliasmesh_t    *dst_mesh;
    maliastc_t      *dst_tc;
    int             i, j, k, val, ret;
    uint16_t        remap[TESS_MAX_INDICES];
    uint16_t        vertIndices[TESS_MAX_INDICES];
    uint16_t        tcIndices[TESS_MAX_INDICES];
    uint16_t        finalIndices[TESS_MAX_INDICES];
    int             numverts, numindices;
    char            skinname[MAX_QPATH];
    vec_t           scale_s, scale_t;
    vec3_t          mins, maxs;

    if (length < sizeof(header)) {
        return Q_ERR_FILE_TOO_SMALL;
    }

    // byte swap the header
    LittleBlock(&header, rawdata, sizeof(header));

    // validate the header
    ret = MOD_ValidateMD2(&header, length);
    if (ret) {
        if (ret == Q_ERR_TOO_FEW) {
            // empty models draw nothing
            model->type = MOD_EMPTY;
            return Q_ERR_SUCCESS;
        }
        return ret;
    }

    // load all triangle indices
    numindices = 0;
    src_tri = (dmd2triangle_t *)((byte *)rawdata + header.ofs_tris);
    for (i = 0; i < header.num_tris; i++) {
        for (j = 0; j < 3; j++) {
            uint16_t idx_xyz = LittleShort(src_tri->index_xyz[j]);
            uint16_t idx_st = LittleShort(src_tri->index_st[j]);

            // some broken models have 0xFFFF indices
            if (idx_xyz >= header.num_xyz || idx_st >= header.num_st) {
                break;
            }

            vertIndices[numindices + j] = idx_xyz;
            tcIndices[numindices + j] = idx_st;
        }
        if (j == 3) {
            // only count good triangles
            numindices += 3;
        }
        src_tri++;
    }

    if (numindices < 3) {
        return Q_ERR_TOO_FEW;
    }

    for (i = 0; i < numindices; i++) {
        remap[i] = 0xFFFF;
    }

    // remap all triangle indices
    numverts = 0;
    src_tc = (dmd2stvert_t *)((byte *)rawdata + header.ofs_st);
    for (i = 0; i < numindices; i++) {
        if (remap[i] != 0xFFFF) {
            continue; // already remapped
        }

        for (j = i + 1; j < numindices; j++) {
            if (vertIndices[i] == vertIndices[j] &&
                (src_tc[tcIndices[i]].s == src_tc[tcIndices[j]].s &&
                 src_tc[tcIndices[i]].t == src_tc[tcIndices[j]].t)) {
                // duplicate vertex
                remap[j] = i;
                finalIndices[j] = numverts;
            }
        }

        // new vertex
        remap[i] = i;
        finalIndices[i] = numverts++;
    }

    if (numverts > TESS_MAX_VERTICES) {
        return Q_ERR_TOO_MANY;
    }

    Hunk_Begin(&model->hunk, 0x400000);
    model->type = MOD_ALIAS;
    model->nummeshes = 1;
    model->numframes = header.num_frames;
    CHECK(model->meshes = MOD_Malloc(sizeof(maliasmesh_t)));
    CHECK(model->frames = MOD_Malloc(header.num_frames * sizeof(maliasframe_t)));

    dst_mesh = model->meshes;
    dst_mesh->numtris = numindices / 3;
    dst_mesh->numindices = numindices;
    dst_mesh->numverts = numverts;
    dst_mesh->numskins = header.num_skins;
    CHECK(dst_mesh->verts = MOD_Malloc(numverts * header.num_frames * sizeof(maliasvert_t)));
    CHECK(dst_mesh->tcoords = MOD_Malloc(numverts * sizeof(maliastc_t)));
    CHECK(dst_mesh->indices = MOD_Malloc(numindices * sizeof(QGL_INDEX_TYPE)));

    if (dst_mesh->numtris != header.num_tris) {
        Com_DPrintf("%s has %d bad triangles\n", model->name, header.num_tris - dst_mesh->numtris);
    }

    // store final triangle indices
    for (i = 0; i < numindices; i++) {
        dst_mesh->indices[i] = finalIndices[i];
    }

    // load all skins
    src_skin = (char *)rawdata + header.ofs_skins;
    for (i = 0; i < header.num_skins; i++) {
        if (!Q_memccpy(skinname, src_skin, 0, sizeof(skinname))) {
            ret = Q_ERR_STRING_TRUNCATED;
            goto fail;
        }
        FS_NormalizePath(skinname);
        dst_mesh->skins[i] = IMG_Find(skinname, IT_SKIN, IF_NONE);
        src_skin += MD2_MAX_SKINNAME;
    }

    // load all tcoords
    src_tc = (dmd2stvert_t *)((byte *)rawdata + header.ofs_st);
    dst_tc = dst_mesh->tcoords;
    scale_s = 1.0f / header.skinwidth;
    scale_t = 1.0f / header.skinheight;
    for (i = 0; i < numindices; i++) {
        if (remap[i] != i) {
            continue;
        }
        dst_tc[finalIndices[i]].st[0] =
            (int16_t)LittleShort(src_tc[tcIndices[i]].s) * scale_s;
        dst_tc[finalIndices[i]].st[1] =
            (int16_t)LittleShort(src_tc[tcIndices[i]].t) * scale_t;
    }

    // load all frames
    src_frame = (dmd2frame_t *)((byte *)rawdata + header.ofs_frames);
    dst_frame = model->frames;
    for (j = 0; j < header.num_frames; j++) {
        LittleVector(src_frame->scale, dst_frame->scale);
        LittleVector(src_frame->translate, dst_frame->translate);

        // load frame vertices
        ClearBounds(mins, maxs);
        for (i = 0; i < numindices; i++) {
            if (remap[i] != i) {
                continue;
            }
            src_vert = &src_frame->verts[vertIndices[i]];
            dst_vert = &dst_mesh->verts[j * numverts + finalIndices[i]];

            dst_vert->pos[0] = src_vert->v[0];
            dst_vert->pos[1] = src_vert->v[1];
            dst_vert->pos[2] = src_vert->v[2];

            val = src_vert->lightnormalindex;
            if (val >= NUMVERTEXNORMALS) {
                dst_vert->norm[0] = 0;
                dst_vert->norm[1] = 0;
            } else {
                dst_vert->norm[0] = gl_static.latlngtab[val][0];
                dst_vert->norm[1] = gl_static.latlngtab[val][1];
            }

            for (k = 0; k < 3; k++) {
                val = dst_vert->pos[k];
                if (val < mins[k])
                    mins[k] = val;
                if (val > maxs[k])
                    maxs[k] = val;
            }
        }

        VectorVectorScale(mins, dst_frame->scale, mins);
        VectorVectorScale(maxs, dst_frame->scale, maxs);

        dst_frame->radius = RadiusFromBounds(mins, maxs);

        VectorAdd(mins, dst_frame->translate, dst_frame->bounds[0]);
        VectorAdd(maxs, dst_frame->translate, dst_frame->bounds[1]);

        src_frame = (dmd2frame_t *)((byte *)src_frame + header.framesize);
        dst_frame++;
    }

    Hunk_End(&model->hunk);
    return Q_ERR_SUCCESS;

fail:
    Hunk_Free(&model->hunk);
    return ret;
}

#if USE_MD3
static int MOD_LoadMD3Mesh(model_t *model, maliasmesh_t *mesh,
                           const byte *rawdata, size_t length, size_t *offset_p)
{
    dmd3mesh_t      header;
    size_t          end;
    dmd3vertex_t    *src_vert;
    dmd3coord_t     *src_tc;
    dmd3skin_t      *src_skin;
    uint32_t        *src_idx;
    maliasvert_t    *dst_vert;
    maliastc_t      *dst_tc;
    QGL_INDEX_TYPE  *dst_idx;
    uint32_t        index;
    char            skinname[MAX_QPATH];
    int             i, j, k, ret;

    if (length < sizeof(header))
        return Q_ERR_BAD_EXTENT;

    // byte swap the header
    LittleBlock(&header, rawdata, sizeof(header));

    if (header.meshsize < sizeof(header) || header.meshsize > length)
        return Q_ERR_BAD_EXTENT;
    if (header.meshsize % q_alignof(dmd3mesh_t))
        return Q_ERR_BAD_ALIGN;
    if (header.num_verts < 3)
        return Q_ERR_TOO_FEW;
    if (header.num_verts > TESS_MAX_VERTICES)
        return Q_ERR_TOO_MANY;
    if (header.num_tris < 1)
        return Q_ERR_TOO_FEW;
    if (header.num_tris > TESS_MAX_INDICES / 3)
        return Q_ERR_TOO_MANY;
    if (header.num_skins > MAX_ALIAS_SKINS)
        return Q_ERR_TOO_MANY;
    end = header.ofs_skins + header.num_skins * sizeof(dmd3skin_t);
    if (end < header.ofs_skins || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header.ofs_skins % q_alignof(dmd3skin_t))
        return Q_ERR_BAD_ALIGN;
    end = header.ofs_verts + header.num_verts * model->numframes * sizeof(dmd3vertex_t);
    if (end < header.ofs_verts || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header.ofs_verts % q_alignof(dmd3vertex_t))
        return Q_ERR_BAD_ALIGN;
    end = header.ofs_tcs + header.num_verts * sizeof(dmd3coord_t);
    if (end < header.ofs_tcs || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header.ofs_tcs % q_alignof(dmd3coord_t))
        return Q_ERR_BAD_ALIGN;
    end = header.ofs_indexes + header.num_tris * 3 * sizeof(uint32_t);
    if (end < header.ofs_indexes || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header.ofs_indexes & 3)
        return Q_ERR_BAD_ALIGN;

    mesh->numtris = header.num_tris;
    mesh->numindices = header.num_tris * 3;
    mesh->numverts = header.num_verts;
    mesh->numskins = header.num_skins;
    CHECK(mesh->verts = MOD_Malloc(sizeof(maliasvert_t) * header.num_verts * model->numframes));
    CHECK(mesh->tcoords = MOD_Malloc(sizeof(maliastc_t) * header.num_verts));
    CHECK(mesh->indices = MOD_Malloc(sizeof(QGL_INDEX_TYPE) * header.num_tris * 3));

    // load all skins
    src_skin = (dmd3skin_t *)(rawdata + header.ofs_skins);
    for (i = 0; i < header.num_skins; i++) {
        if (!Q_memccpy(skinname, src_skin->name, 0, sizeof(skinname)))
            return Q_ERR_STRING_TRUNCATED;
        FS_NormalizePath(skinname);
        mesh->skins[i] = IMG_Find(skinname, IT_SKIN, IF_NONE);
        src_skin++;
    }

    // load all vertices
    src_vert = (dmd3vertex_t *)(rawdata + header.ofs_verts);
    dst_vert = mesh->verts;
    for (i = 0; i < model->numframes; i++) {
        maliasframe_t *f = &model->frames[i];

        for (j = 0; j < header.num_verts; j++) {
            dst_vert->pos[0] = (int16_t)LittleShort(src_vert->point[0]);
            dst_vert->pos[1] = (int16_t)LittleShort(src_vert->point[1]);
            dst_vert->pos[2] = (int16_t)LittleShort(src_vert->point[2]);

            dst_vert->norm[0] = src_vert->norm[0];
            dst_vert->norm[1] = src_vert->norm[1];

            for (k = 0; k < 3; k++) {
                f->bounds[0][k] = min(f->bounds[0][k], dst_vert->pos[k]);
                f->bounds[1][k] = max(f->bounds[1][k], dst_vert->pos[k]);
            }

            src_vert++; dst_vert++;
        }
    }

    // load all texture coords
    src_tc = (dmd3coord_t *)(rawdata + header.ofs_tcs);
    dst_tc = mesh->tcoords;
    for (i = 0; i < header.num_verts; i++) {
        dst_tc->st[0] = LittleFloat(src_tc->st[0]);
        dst_tc->st[1] = LittleFloat(src_tc->st[1]);
        src_tc++; dst_tc++;
    }

    // load all triangle indices
    src_idx = (uint32_t *)(rawdata + header.ofs_indexes);
    dst_idx = mesh->indices;
    for (i = 0; i < header.num_tris * 3; i++) {
        index = LittleLong(*src_idx++);
        if (index >= header.num_verts)
            return Q_ERR_BAD_INDEX;
        *dst_idx++ = index;
    }

    *offset_p = header.meshsize;
    return Q_ERR_SUCCESS;

fail:
    return ret;
}

static int MOD_LoadMD3(model_t *model, const void *rawdata, size_t length)
{
    dmd3header_t    header;
    size_t          end, offset, remaining;
    dmd3frame_t     *src_frame;
    maliasframe_t   *dst_frame;
    const byte      *src_mesh;
    int             i, ret;

    if (length < sizeof(header))
        return Q_ERR_FILE_TOO_SMALL;

    // byte swap the header
    LittleBlock(&header, rawdata, sizeof(header));

    if (header.ident != MD3_IDENT)
        return Q_ERR_UNKNOWN_FORMAT;
    if (header.version != MD3_VERSION)
        return Q_ERR_UNKNOWN_FORMAT;
    if (header.num_frames < 1)
        return Q_ERR_TOO_FEW;
    if (header.num_frames > MD3_MAX_FRAMES)
        return Q_ERR_TOO_MANY;
    end = header.ofs_frames + sizeof(dmd3frame_t) * header.num_frames;
    if (end < header.ofs_frames || end > length)
        return Q_ERR_BAD_EXTENT;
    if (header.ofs_frames % q_alignof(dmd3frame_t))
        return Q_ERR_BAD_ALIGN;
    if (header.num_meshes < 1)
        return Q_ERR_TOO_FEW;
    if (header.num_meshes > MD3_MAX_MESHES)
        return Q_ERR_TOO_MANY;
    if (header.ofs_meshes > length)
        return Q_ERR_BAD_EXTENT;
    if (header.ofs_meshes % q_alignof(dmd3mesh_t))
        return Q_ERR_BAD_ALIGN;

    Hunk_Begin(&model->hunk, 0x400000);
    model->type = MOD_ALIAS;
    model->numframes = header.num_frames;
    model->nummeshes = header.num_meshes;
    CHECK(model->meshes = MOD_Malloc(sizeof(maliasmesh_t) * header.num_meshes));
    CHECK(model->frames = MOD_Malloc(sizeof(maliasframe_t) * header.num_frames));

    // load all frames
    src_frame = (dmd3frame_t *)((byte *)rawdata + header.ofs_frames);
    dst_frame = model->frames;
    for (i = 0; i < header.num_frames; i++) {
        LittleVector(src_frame->translate, dst_frame->translate);
        VectorSet(dst_frame->scale, MD3_XYZ_SCALE, MD3_XYZ_SCALE, MD3_XYZ_SCALE);

        ClearBounds(dst_frame->bounds[0], dst_frame->bounds[1]);

        src_frame++; dst_frame++;
    }

    // load all meshes
    src_mesh = (const byte *)rawdata + header.ofs_meshes;
    remaining = length - header.ofs_meshes;
    for (i = 0; i < header.num_meshes; i++) {
        ret = MOD_LoadMD3Mesh(model, &model->meshes[i], src_mesh, remaining, &offset);
        if (ret)
            goto fail;
        src_mesh += offset;
        remaining -= offset;
    }

    // calculate frame bounds
    dst_frame = model->frames;
    for (i = 0; i < header.num_frames; i++) {
        VectorScale(dst_frame->bounds[0], MD3_XYZ_SCALE, dst_frame->bounds[0]);
        VectorScale(dst_frame->bounds[1], MD3_XYZ_SCALE, dst_frame->bounds[1]);

        dst_frame->radius = RadiusFromBounds(dst_frame->bounds[0], dst_frame->bounds[1]);

        VectorAdd(dst_frame->bounds[0], dst_frame->translate, dst_frame->bounds[0]);
        VectorAdd(dst_frame->bounds[1], dst_frame->translate, dst_frame->bounds[1]);

        dst_frame++;
    }

    Hunk_End(&model->hunk);
    return Q_ERR_SUCCESS;

fail:
    Hunk_Free(&model->hunk);
    return ret;
}
#endif

void MOD_Reference(model_t *model)
{
    int i, j;

    // register any images used by the models
    switch (model->type) {
    case MOD_ALIAS:
        for (i = 0; i < model->nummeshes; i++) {
            maliasmesh_t *mesh = &model->meshes[i];
            for (j = 0; j < mesh->numskins; j++) {
                mesh->skins[j]->registration_sequence = registration_sequence;
            }
        }
        break;
    case MOD_SPRITE:
        for (i = 0; i < model->numframes; i++) {
            model->spriteframes[i].image->registration_sequence = registration_sequence;
        }
        break;
    case MOD_EMPTY:
        break;
    default:
        Com_Error(ERR_FATAL, "%s: bad model type", __func__);
    }

    model->registration_sequence = registration_sequence;
}

qhandle_t R_RegisterModel(const char *name)
{
    char normalized[MAX_QPATH];
    qhandle_t index;
    size_t namelen;
    int filelen;
    model_t *model;
    byte *rawdata;
    uint32_t ident;
    int (*load)(model_t *, const void *, size_t);
    int ret;

    // empty names are legal, silently ignore them
    if (!*name)
        return 0;

    if (*name == '*') {
        // inline bsp model
        index = atoi(name + 1);
        return ~index;
    }

    // normalize the path
    namelen = FS_NormalizePathBuffer(normalized, name, MAX_QPATH);

    // this should never happen
    if (namelen >= MAX_QPATH)
        Com_Error(ERR_DROP, "%s: oversize name", __func__);

    // normalized to empty name?
    if (namelen == 0) {
        Com_DPrintf("%s: empty name\n", __func__);
        return 0;
    }

    // see if it's already loaded
    model = MOD_Find(normalized);
    if (model) {
        MOD_Reference(model);
        goto done;
    }

    filelen = FS_LoadFile(normalized, (void **)&rawdata);
    if (!rawdata) {
        // don't spam about missing models
        if (filelen == Q_ERR_NOENT) {
            return 0;
        }

        ret = filelen;
        goto fail1;
    }

    if (filelen < 4) {
        ret = Q_ERR_FILE_TOO_SMALL;
        goto fail2;
    }

    // check ident
    ident = LittleLong(*(uint32_t *)rawdata);
    switch (ident) {
    case MD2_IDENT:
        load = MOD_LoadMD2;
        break;
#if USE_MD3
    case MD3_IDENT:
        load = MOD_LoadMD3;
        break;
#endif
    case SP2_IDENT:
        load = MOD_LoadSP2;
        break;
    default:
        ret = Q_ERR_UNKNOWN_FORMAT;
        goto fail2;
    }

    model = MOD_Alloc();
    if (!model) {
        ret = Q_ERR_OUT_OF_SLOTS;
        goto fail2;
    }

    memcpy(model->name, normalized, namelen + 1);
    model->registration_sequence = registration_sequence;

    ret = load(model, rawdata, filelen);

    FS_FreeFile(rawdata);

    if (ret) {
        memset(model, 0, sizeof(*model));
        goto fail1;
    }

done:
    index = (model - r_models) + 1;
    return index;

fail2:
    FS_FreeFile(rawdata);
fail1:
    Com_EPrintf("Couldn't load %s: %s\n", normalized, Q_ErrorString(ret));
    return 0;
}

model_t *MOD_ForHandle(qhandle_t h)
{
    model_t *model;

    if (!h) {
        return NULL;
    }

    if (h < 0 || h > r_numModels) {
        Com_Error(ERR_DROP, "%s: %d out of range", __func__, h);
    }

    model = &r_models[h - 1];
    if (!model->type) {
        return NULL;
    }

    return model;
}

void MOD_Init(void)
{
    if (r_numModels) {
        Com_Error(ERR_FATAL, "%s: %d models not freed", __func__, r_numModels);
    }

    Cmd_AddCommand("modellist", MOD_List_f);
}

void MOD_Shutdown(void)
{
    MOD_FreeAll();
    Cmd_RemoveCommand("modellist");
}
