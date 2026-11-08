/* SPDX-License-Identifier: GPL-2.0-only */
/* PMI8998 QC policy, following Xiaomi's SMB2 limits and IRQ workaround.
 * Battery FCC/FV/JEITA and the Type-C/OTG regulator remain independently owned.
 * The caller holds policy_lock. No negotiation is started on SDP/CDP cables.
 */
#define CHIRON_QC_CMD                 0x343
#define CHIRON_QC_FORCE_5V            BIT(3)
#define CHIRON_QC_PULSE_MAX           0x35b
#define CHIRON_QC_ADAPTER_ALLOW       0x360
#define CHIRON_QC_SOURCE_IRQ          0x369
#define CHIRON_QC_AUTH_IRQ            BIT(2)
#define CHIRON_QC_VADP_IRQ            BIT(3)
#define CHIRON_QC_BUCK_FREQ           0x6a0
#define CHIRON_QC_AUTH_AUTO           (HVDCP_AUTH_ALG_EN_CFG_BIT | \
				      HVDCP_AUTONOMOUS_MODE_EN_CFG_BIT)

static int smb_chiron_qc_set_buck(struct smb_chip *chip, unsigned int freq)
{
	const struct reg_sequence seq[] = {
		{ chip->base + 0x6d0, 0xa5 },
		{ chip->base + CHIRON_QC_BUCK_FREQ, freq },
	};
	unsigned int actual;
	int rc;

	/* Hold the shared regmap lock over unlock + protected write. */
	rc = regmap_multi_reg_write(chip->regmap, seq, ARRAY_SIZE(seq));
	if (rc)
		return rc;
	rc = regmap_read(chip->regmap, chip->base + CHIRON_QC_BUCK_FREQ, &actual);
	if (rc)
		return rc;
	return actual == freq ? 0 : -EIO;
}

static int smb_chiron_qc_stop(struct smb_chip *chip)
{
	int rc, first_rc = 0, online = 0;

	rc = smb_chiron_fast_reset(chip);
	if (rc) {
		dev_err_ratelimited(chip->dev, "QC FCC reset failed: %d\n", rc);
		first_rc = rc;
	}
	chip->qc_active = false;
	chip->qc_authenticated = false;
	rc = regmap_update_bits(chip->regmap, chip->base + USBIN_OPTIONS_1_CFG,
				CHIRON_QC_AUTH_AUTO, 0);
	if (rc)
		return first_rc ? first_rc : rc;
	rc = smb_get_prop_usb_online(chip, &online);
	if (rc)
		return first_rc ? first_rc : rc;
	if (online) {
		rc = regmap_write(chip->regmap, chip->base + CHIRON_QC_CMD,
				  CHIRON_QC_FORCE_5V);
		if (rc)
			return first_rc ? first_rc : rc;
	}
	/* Stock removal/transition frequency: 1000 kHz, 600 + 2 * 200. */
	rc = smb_chiron_qc_set_buck(chip, 2);
	return first_rc ? first_rc : rc;
}

static int smb_chiron_qc_start(struct smb_chip *chip)
{
	unsigned int health;
	int rc;

	chip->qc_attempted = true;
	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_2,
			 &health);
	if (rc)
		return rc;
	if (health & ~INPUT_CURRENT_LIMITED_BIT)
		return 0;
	rc = smb_chiron_input_limit(chip, SDP_CURRENT_UA);
	if (rc)
		return rc;
	/* Stock QC2 <= 9 V and QC3 <= 8 pulses (5 V + 8 * 0.2 V). */
	rc = regmap_write(chip->regmap, chip->base + CHIRON_QC_PULSE_MAX, 0x48);
	if (rc)
		return rc;
	rc = regmap_update_bits(chip->regmap, chip->base + CHIRON_QC_ADAPTER_ALLOW,
				0xf, 8);
	if (rc)
		return rc;
	rc = smb_chiron_qc_set_buck(chip, 2);
	if (rc)
		return rc;
	/* PMI8998 needs AUTH IRQ until authentication, then VADP IRQ. */
	rc = regmap_update_bits(chip->regmap, chip->base + CHIRON_QC_SOURCE_IRQ,
				GENMASK(3, 0), GENMASK(3, 0));
	if (rc)
		return rc;
	rc = regmap_update_bits(chip->regmap, chip->base + USBIN_OPTIONS_1_CFG,
				CHIRON_QC_AUTH_AUTO | HVDCP_EN_BIT,
				CHIRON_QC_AUTH_AUTO | HVDCP_EN_BIT);
	if (rc)
		return rc;
	chip->qc_active = true;
	chip->qc_deadline = jiffies + msecs_to_jiffies(20000);
	rc = regmap_update_bits(chip->regmap, chip->base + CMD_APSD,
				APSD_RERUN_BIT, APSD_RERUN_BIT);
	if (!rc)
		mod_delayed_work(system_dfl_wq, &chip->qc_work, msecs_to_jiffies(1000));
	return rc;
}

static void smb_chiron_qc_work(struct work_struct *work)
{
	struct smb_chip *chip = container_of(to_delayed_work(work),
					    struct smb_chip, qc_work);
	unsigned int status, result, health, freq;
	int rc, online = 0, uv;
	bool retry = false;

	mutex_lock(&chip->policy_lock);
	if (chip->stopping || !chip->qc_active)
		goto out;
	rc = smb_get_prop_usb_online(chip, &online);
	if (rc || !online)
		goto fallback;
	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_2,
			 &health);
	if (rc || (health & ~INPUT_CURRENT_LIMITED_BIT))
		goto fallback;
	rc = regmap_read(chip->regmap, chip->base + APSD_STATUS, &status);
	if (rc)
		goto fallback;
	rc = regmap_read(chip->regmap, chip->base + APSD_RESULT_STATUS, &result);
	if (rc)
		goto fallback;
	rc = iio_read_channel_processed(chip->usb_in_v_chan, &uv);
	if (rc < 0 || uv > 9500000 || uv < 3500000)
		goto fallback;
	if ((status & QC_AUTH_DONE_STATUS_BIT) &&
	    (result & (QC_2P0_BIT | QC_3P0_BIT))) {
		rc = regmap_update_bits(chip->regmap, chip->base + CHIRON_QC_SOURCE_IRQ,
					CHIRON_QC_AUTH_IRQ | CHIRON_QC_VADP_IRQ,
					CHIRON_QC_VADP_IRQ);
		if (rc)
			goto fallback;
		/* Downstream buckets: 600 kHz at 5 V, 800 at 6-8 V, 1000 at 9 V. */
		freq = uv < 6000000 ? 0 : uv < 8500000 ? 1 : 2;
		rc = smb_chiron_qc_set_buck(chip, freq);
		if (rc)
			goto fallback;
		rc = smb_chiron_input_limit(chip, chip->fast_icl);
		if (rc)
			goto fallback;
		if (!chip->qc_authenticated)
			dev_info(chip->dev, "QC%s authenticated, VBUS %d uV\n",
				 result & QC_3P0_BIT ? "3" : "2", uv);
		chip->qc_authenticated = true;
		/* Subsequent voltage changes are handled by source-change IRQ. */
		goto changed;
	}
	if (chip->qc_authenticated)
		goto fallback;
	if (time_after_eq(jiffies, chip->qc_deadline)) {
		/* Only a healthy negotiation timeout gets one delayed retry.
		 * Cable, sensor and protection failures never take this path.
		 */
		retry = chip->qc_retries == 0;
		goto fallback;
	}
	mod_delayed_work(system_dfl_wq, &chip->qc_work, msecs_to_jiffies(1000));
	goto changed;
fallback:
	rc = smb_chiron_qc_stop(chip);
	if (rc)
		dev_err_ratelimited(chip->dev, "QC fallback failed: %d\n", rc);
	if (!rc && retry) {
		chip->qc_retries++;
		chip->qc_attempted = false;
		chip->qc_prepare_deadline = jiffies + msecs_to_jiffies(20000);
		dev_info(chip->dev, "QC timeout: one retry after 20 seconds\n");
	}
	/* Restore ordinary charging during the bounded retry cooldown. */
	mod_delayed_work(system_dfl_wq, &chip->status_change_work, msecs_to_jiffies(1000));
changed:
	power_supply_changed(chip->chg_psy);
out:
	mutex_unlock(&chip->policy_lock);
}

static irqreturn_t smb_chiron_source_irq(int irq, void *data)
{
	struct smb_chip *chip = data;

	mutex_lock(&chip->policy_lock);
	if (!chip->stopping) {
		mod_delayed_work(system_dfl_wq, &chip->qc_work, 0);
		mod_delayed_work(system_dfl_wq, &chip->status_change_work,
				 msecs_to_jiffies(250));
	}
	mutex_unlock(&chip->policy_lock);
	return IRQ_HANDLED;
}
