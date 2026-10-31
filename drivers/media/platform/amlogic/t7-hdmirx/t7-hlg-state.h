/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T7_HLG_STATE_H
#define T7_HLG_STATE_H

/* CTA static metadata EOTF values. A changed encoding invalidates the lease;
 * no frame from the new encoding may use the previous HDR2 configuration.
 */
static inline bool t7_hdr_eotf_supported(unsigned int eotf)
{
	return eotf == 2 || eotf == 3;
}

static inline bool t7_hdr_encoding_changed(bool hdr, unsigned int eotf,
					   bool leased_hdr, unsigned int leased_eotf)
{
	return hdr != leased_hdr || (hdr && eotf != leased_eotf);
}
#endif
