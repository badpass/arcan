/*
 Arcan Shared Memory Interface, Extended Mapping

 Friendly Warning:
 These are extended internal sub-protocols only used for segmenting the
 engine into multiple processes. It relies on data-types not defined in
 the rest of shmif and is therefore wholly unsuitable for inclusion or
 use in code elsewhere.
 */

/*
 Copyright (c) 2016-2017, Bjorn Stahl
 All rights reserved.

 Redistribution and use in source and binary forms,
 with or without modification, are permitted provided that the
 following conditions are met:

 1. Redistributions of source code must retain the above copyright notice,
 this list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 3. Neither the name of the copyright holder nor the names of its contributors
 may be used to endorse or promote products derived from this software without
 specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef HAVE_ARCAN_SHMIF_SUBPROTO
#define HAVE_ARCAN_SHMIF_SUBPROTO

/*
 * BSD checksum, from BSD sum. We calculate this to get lock-free updates while
 * staying away from atomic/reordering (since there's no atomic 64- byte type
 * anyhow). The added gain is that when metadata is synched and we're in a
 * partial update, the failed checksum means there is new data on the way
 * anyhow or something more sinister is afoot.
 */
static inline uint16_t subp_checksum(uint8_t* buf, size_t len)
{
	uint16_t res = 0;
	for (size_t i = 0; i < len; i++){
		if (res & 1)
			res |= 0x10000;
		res = ((res >> 1) + buf[i]) & 0xffff;
	}
	return res;
}

/*
 * Forward declarations of the current sub- structures, and a union of
 * the safe / known return pointers in order to avoid explicit casting
 */
struct arcan_shmif_vr;
struct arcan_shmif_ramp;
struct arcan_shmif_hdr16f;
struct arcan_shmif_vector;

union shmif_ext_substruct {
	struct arcan_shmif_vr* vr;
	struct arcan_shmif_ramp* cramp;
	struct arcan_shmif_hdr16f* hdr;
	struct arcan_shmif_vector* vector;
};

/*
 * Extract a valid sub-structure from the context. This should have been
 * negotiated with an extended resize request in advance, and need to be
 * re- extracted in the event of an extended meta renegotiation, a reset
 * or a migration. The safest pattern is to simply call when the data is
 * needed and never cache.
 */
union shmif_ext_substruct arcan_shmif_substruct(
	struct arcan_shmif_cont* ctx, enum shmif_ext_meta meta);

/*
 * Marks the beginning of the offset table that is set if subprotocols
 * have been activated. Used internally by the resize- function. The
 * strict copy is kept server-side.
 */
struct arcan_shmif_ofstbl {
	union {
	struct {
		uint32_t ofs_ramp, sz_ramp;
		uint32_t ofs_vr, sz_vr;
		uint32_t ofs_hdr, sz_hdr;
		uint32_t ofs_vector, sz_vector;
	};
	uint32_t offsets[32];
	};
};

struct arcan_shmif_hdr16f {
	int unused;
};
struct arcan_shmif_vector {
	int unused;
};

/*
 * a crutch with this approach is that we need to relocate when mapping in the
 * resize handler, though the _shmifsub_getramp/setramp functions will mitigate
 * this.
 */
struct ramp_block {
	bool output;
	uint8_t format;
	size_t plane_size;

	uint8_t edid[128];
	uint16_t checksum;

/* 3 * plane_size */
	uint16_t planes[0];
};

struct arcan_shmif_ramp {
/* BITMASK, PRODUCER SET, CONSUMER CLEAR */
	_Atomic uint_least8_t dirty_in;

/* BITMASK, CONSUMER SET, PRODUCER CLEAR */
	_Atomic uint_least8_t dirty_out;

/* PRODUCER INIT */
	uint8_t n_blocks;

/* PRODUCER INIT, CONSUMER_UPDATE */
	struct ramp_block ramps[];
};

/*
 * retrieve the metadata on the current ramp-block,
 * returns the number of ramps (or -1 if no data could be retrieved)
 * nd [optionally] display EDID
 */
ssize_t arcan_shmifsub_rampmeta(struct arcan_shmif_cont* cont,
	uint8_t** out_edid, size_t* edid_sz);

bool arcan_shmifsub_getramp(
	struct arcan_shmif_cont* cont, size_t ind, struct ramp_block* out);

bool arcan_shmifsub_setramp(
	struct arcan_shmif_cont* cont, size_t ind, struct ramp_block* in);

/*
 * To avoid namespace collisions, the detailed VR structure relies
 * on having access to the definitions in arcan_math.h from the core
 * engine code.
 */
#ifdef HAVE_ARCAN_MATH
#define VR_VERSION 0x1000

/*
 * This structure is mapped into the adata area. It can be verified
 * if the apad value match the size and the apad_type matches the
 * SHMIF_APAD_VR constant.
 */
enum avatar_limbs {
	PERSON = 0, /* abstract for global positioning */
	NECK,
	L_EYE,
	R_EYE,
	L_SHOULDER,
	R_SHOULDER,
	L_ELBOW,
	R_ELBOW,
	L_WRIST,
	R_WRIST,
/* might seem overly detailed but with glove- devices some points
 * can be sampled and others can be inferred through kinematics */
	L_THUMB_PROXIMAL,
	L_THUMB_MIDDLE,
	L_THUMB_DISTAL,
	L_POINTER_PROXIMAL,
	L_POINTER_MIDDLE,
	L_POINTER_DISTAL,
	L_MIDDLE_PROXIMAL,
	L_MIDDLE_MIDDLE,
	L_MIDDLE_DISTAL,
	L_RING_PROXIMAL,
	L_RING_MIDDLE,
	L_RING_DISTAL,
	L_PINKY_PROXIMAL,
	L_PINKY_MIDDLE,
	L_PINKY_DISTAL,
	R_THUMB_PROXIMAL,
	R_THUMB_MIDDLE,
	R_THUMB_DISTAL,
	R_POINTER_PROXIMAL,
	R_POINTER_MIDDLE,
	R_POINTER_DISTAL,
	R_MIDDLE_PROXIMAL,
	R_MIDDLE_MIDDLE,
	R_MIDDLE_DISTAL,
	R_RING_PROXIMAL,
	R_RING_MIDDLE,
	R_RING_DISTAL,
	R_PINKY_PROXIMAL,
	R_PINKY_MIDDLE,
	R_PINKY_DISTAL,
	L_HIP,
	R_HIP,
	L_KNEE,
	R_KNEE,
	L_ANKLE,
	R_ANKLE,
	LIMB_LIM
};

/*
 * Special TARGET_COMMAND... handling:
 * BCHUNKSTATE:
 *  extension 'arcan_vr_distort' for distortion mesh packed as native
 *  floats with a header indicating elements then elements*[X,Y,Z],elements*[S,T]
 *
 * IO- events are used to activate haptics
 */

/*
 * The standard lens parameters
 */
struct vr_meta {
/* pixels */
	unsigned hres;
	unsigned vres;

/* values in meters to keep < 1.0 */
	float h_size;
	float v_size;
	float h_center;
	float eye_display;
	float lens_distance;
	float ipd;

/* correction constants */
	float distortion[4];
	float abberation[4];
};

struct vr_limb {
/* CONSUMER-SET: don't bother updating, won't be used. */
	bool ignored;

/* PRODUCER_SET (activation) */
	enum avatar_limbs limb_type;

/* PRODUCER_SET (activation, or 0 if no haptics) */
	uint32_t haptic_id;
	uint32_t haptic_capabilities;

/* PRODUCER_UPDATE */
	_Atomic uint_least32_t timestamp;

/* PRODUCER UPDATE */
	union {
		uint8_t data[64];
		struct {
			vector position;
			vector forward;
			quat orientation;
			uint16_t checksum;
		};
	};
};

/*
 * 0 <= (page_sz) - offset_of(limb) - limb_lim*sizeof(struct) limb
 */
struct arcan_shmif_vr {
/* CONSUMER SET (activation) */
	size_t page_sz;
	uint8_t version;
	uint8_t limb_lim;

/* PRODUCER MODIFY */
	_Atomic uint_least64_t limb_mask;

/* PRODUCER SET */
	_Atomic uint_least8_t ready;

/* PRODUCER INIT */
	struct vr_meta meta;

/* PRODUCER UPDATE (see struct definition) */
	struct vr_limb limbs[];
};
#endif
#endif
