/* SPDX-License-Identifier: ISC */
#ifndef ATH10K_CHIRON_AMSDU_POLICY_H
#define ATH10K_CHIRON_AMSDU_POLICY_H

#define CHIRON_AMSDU_MAX 64U
#define CHIRON_AMSDU_TIMEOUT_MS 100U
#define CHIRON_AMSDU_FIRST 1U
#define CHIRON_AMSDU_LAST 2U

/* Input is the prefix ending at the first LAST, or at the event boundary.
 * Return -1 for invalid, 0 for an incomplete prefix, 1 for a complete one.
 * MPDU sequence/TID fields are not valid on non-FIRST descriptors.
 */
static inline int ath10k_chiron_amsdu_prefix(const unsigned char *flags,
                                           unsigned int count,
                                           unsigned int held,
                                           int continuation)
{
    unsigned int i;

    if (!count || count > CHIRON_AMSDU_MAX || held > CHIRON_AMSDU_MAX ||
        count > CHIRON_AMSDU_MAX - held)
        return -1;
    if (!!(flags[0] & CHIRON_AMSDU_FIRST) == !!continuation)
        return -1;
    for (i = 0; i < count; i++) {
        if (i && (flags[i] & CHIRON_AMSDU_FIRST))
            return -1;
        if (flags[i] & CHIRON_AMSDU_LAST)
            return 1;
    }
    return 0;
}

#endif
