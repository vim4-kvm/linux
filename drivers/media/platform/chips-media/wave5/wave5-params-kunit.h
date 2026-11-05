/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WAVE5_PARAMS_KUNIT_H
#define WAVE5_PARAMS_KUNIT_H

#if IS_ENABLED(CONFIG_KUNIT)
void wave5_set_intra_refresh(struct enc_wave_param *param, u32 height,
			     u32 period, bool hevc);
void wave5_set_enc_picture_size(struct enc_open_param *open,
				struct vpu_instance *inst);
int wave5_set_vbv_size(struct enc_open_param *open, struct vpu_instance *inst);
int wave5_set_enc_gop(struct enc_open_param *open, struct vpu_instance *inst);
void wave5_set_enc_crop_info(u32 codec, struct enc_wave_param *param, int rot_mode,
			     int src_width, int src_height);
u32 wave5_enc_sps_profile(struct vpu_instance *inst, u32 profile);
u32 wave5_enc_source_endian(enum frame_buffer_format format, bool legacy);
#endif

#endif
