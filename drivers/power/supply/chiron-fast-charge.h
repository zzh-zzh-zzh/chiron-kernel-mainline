/* SPDX-License-Identifier: GPL-2.0-only */
/* Chiron: stock 3.3 A FCC / 2.7 A QC3 input, conservative thermal gating.
 * policy_lock serializes this with cable and QC transitions.
 */
static int smb_chiron_charge_write(struct smb_chip *chip, unsigned int offset,
				   unsigned int value)
{
	const struct reg_sequence seq[] = {
		{ chip->base + 0xd0, 0xa5 },
		{ chip->base + offset, value },
	};
	unsigned int actual;
	int rc;

	rc = regmap_multi_reg_write(chip->regmap, seq, ARRAY_SIZE(seq));
	if (rc)
		return rc;
	rc = regmap_read(chip->regmap, chip->base + offset, &actual);
	return rc ? rc : actual == value ? 0 : -EIO;
}

static int smb_chiron_fast_reset(struct smb_chip *chip)
{
	int rc, first_rc = 0;

	chip->fast_icl = DCP_CURRENT_UA;
	rc = smb_chiron_charge_write(chip, FAST_CHARGE_CURRENT_CFG, 40);
	if (rc)
		first_rc = rc;
	rc = smb_chiron_charge_write(chip, 0x92, chip->jeita_comp);
	if (rc && !first_rc)
		first_rc = rc;
	return first_rc;
}

static void smb_chiron_fast_work(struct work_struct *work)
{
	struct smb_chip *chip = container_of(to_delayed_work(work),
					    struct smb_chip, fast_work);
	union power_supply_propval temp, capacity, present;
	struct power_supply *battery;
	unsigned int health, jeita, fv, fcc, result;
	int rc, online = 0, die_temp;
	bool eligible = false;

	mutex_lock(&chip->policy_lock);
	if (chip->stopping)
		goto out;
	rc = smb_get_prop_usb_online(chip, &online);
	if (rc)
		goto reset;
	rc = smb_chiron_limit_update(chip, online);
	if (rc || !online || chip->charge_held)
		goto reset;
	if (!chip->qc_authenticated)
		goto reset;
	rc = regmap_read(chip->regmap, chip->base + BATTERY_CHARGER_STATUS_2, &health);
	if (rc || (health & ~INPUT_CURRENT_LIMITED_BIT))
		goto reset;
	rc = regmap_read(chip->regmap, chip->base + 0x90, &jeita);
	if (rc || (jeita & 0x1f) != 0x1f)
		goto reset;
	rc = regmap_read(chip->regmap, chip->base + FLOAT_VOLTAGE_CFG, &fv);
	if (rc || fv > 121)
		goto reset;
	rc = iio_read_channel_processed(chip->charger_temp_chan, &die_temp);
	if (rc < 0 || die_temp < 0 || die_temp >= 70000) {
		chip->fast_hot = true;
		goto reset;
	}
	battery = power_supply_get_by_name("chiron-battery");
	if (!battery)
		goto reset;
	rc = power_supply_get_property(battery, POWER_SUPPLY_PROP_TEMP, &temp);
	if (!rc)
		rc = power_supply_get_property(battery, POWER_SUPPLY_PROP_PRESENT, &present);
	/* Capacity succeeds only for the battery driver's stock ID allowlist. */
	if (!rc)
		rc = power_supply_get_property(battery, POWER_SUPPLY_PROP_CAPACITY, &capacity);
	power_supply_put(battery);
	if (rc || !present.intval || temp.intval < 150 || temp.intval >= 400) {
		chip->fast_hot = true;
		goto reset;
	}
	if (chip->fast_hot && (temp.intval > 380 || die_temp > 65000))
		goto reset;
	chip->fast_hot = false;
	rc = regmap_read(chip->regmap, chip->base + APSD_RESULT_STATUS, &result);
	if (rc || !(result & (QC_2P0_BIT | QC_3P0_BIT)))
		goto reset;
	/* At 3.3 A, hardware soft JEITA must immediately reduce to <= 0.5 A.
	 * Do not depend solely on the five-second software thermal check.
	 */
	rc = smb_chiron_charge_write(chip, 0x92, max(chip->jeita_comp, 112U));
	if (rc)
		goto reset;
	chip->fast_icl = result & QC_3P0_BIT ? 2700000 : DCP_CURRENT_UA;
	rc = smb_chiron_input_limit(chip, chip->fast_icl);
	if (rc)
		goto reset;
	rc = regmap_read(chip->regmap, chip->base + FAST_CHARGE_CURRENT_CFG, &fcc);
	if (rc)
		goto reset;
	/* Ramp by at most 0.5 A per sample, with the stock 3.3 A ceiling. */
	rc = smb_chiron_charge_write(chip, FAST_CHARGE_CURRENT_CFG, min(fcc + 20, 132U));
	if (rc)
		goto reset;
	eligible = true;
	goto again;
reset:
	rc = smb_chiron_fast_reset(chip);
	if (rc)
		dev_err_ratelimited(chip->dev, "FCC fallback failed: %d\n", rc);
	if (online && chip->qc_authenticated)
		smb_chiron_input_limit(chip, DCP_CURRENT_UA);
again:
	if (chip->fast_enabled != eligible) {
		chip->fast_enabled = eligible;
		dev_info(chip->dev, "Battery fast charge %s\n", eligible ? "enabled" : "limited");
	}
	if (online)
		mod_delayed_work(system_dfl_wq, &chip->fast_work, msecs_to_jiffies(5000));
out:
	mutex_unlock(&chip->policy_lock);
}
