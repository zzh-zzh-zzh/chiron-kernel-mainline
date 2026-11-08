/* SPDX-License-Identifier: GPL-2.0-only */
/* Restore the charger register ownership and command after a failed transition. */
static int smb_chiron_charge_restore(struct smb_chip *chip,
					     unsigned int cfg, unsigned int command)
{
	struct reg_sequence seq[] = {
		{ chip->base + 0xd0, 0xa5 },
		{ chip->base + CHGR_CFG2, 0 },
	};
	unsigned int actual_cfg, actual_command, restore_cfg;
	int rc;

	rc = regmap_read(chip->regmap, chip->base + CHGR_CFG2, &actual_cfg);
	if (rc)
		return rc;
	restore_cfg = (actual_cfg & ~(CHG_EN_SRC_BIT | CHG_EN_POLARITY_BIT)) |
			(cfg & (CHG_EN_SRC_BIT | CHG_EN_POLARITY_BIT));
	seq[1].def = restore_cfg;
	rc = regmap_multi_reg_write(chip->regmap, seq, ARRAY_SIZE(seq));
	if (rc)
		return rc;
	rc = regmap_update_bits(chip->regmap, chip->base + CHARGING_ENABLE_CMD,
				CHARGING_ENABLE_CMD_BIT,
				command & CHARGING_ENABLE_CMD_BIT);
	if (rc)
		return rc;
	rc = regmap_read(chip->regmap, chip->base + CHGR_CFG2, &actual_cfg);
	if (rc)
		return rc;
	if ((actual_cfg ^ cfg) & (CHG_EN_SRC_BIT | CHG_EN_POLARITY_BIT))
		return -EIO;
	rc = regmap_read(chip->regmap, chip->base + CHARGING_ENABLE_CMD,
			 &actual_command);
	if (rc)
		return rc;
	return ((actual_command ^ command) & CHARGING_ENABLE_CMD_BIT) ? -EIO : 0;
}

/* Stop battery charging without suspending the USB system power path. */
static int smb_chiron_charge_hold(struct smb_chip *chip, bool hold)
{
	unsigned int cfg, initial_cfg, command, initial_command;
	unsigned int old_hw_cfg = chip->charge_hw_cfg;
	bool old_sw_owned = chip->charge_sw_owned;
	bool old_held = chip->charge_held;
	bool sw_owned = chip->charge_sw_owned;
	unsigned int hw_cfg = chip->charge_hw_cfg;
	int rc, restore_rc;

	rc = regmap_read(chip->regmap, chip->base + CHGR_CFG2, &cfg);
	if (rc)
		return rc;
	initial_cfg = cfg;
	rc = regmap_read(chip->regmap, chip->base + CHARGING_ENABLE_CMD,
			 &command);
	if (rc)
		return rc;
	initial_command = command;
	/* Firmware owns the command path already. */
	if ((cfg & CHG_EN_SRC_BIT) && chip->charge_limit == 100)
		return 0;
	if (cfg & CHG_EN_SRC_BIT) {
		const struct reg_sequence seq[] = {
			{ chip->base + 0xd0, 0xa5 },
			{ chip->base + CHGR_CFG2, cfg & ~(CHG_EN_SRC_BIT | CHG_EN_POLARITY_BIT) },
		};

		hw_cfg = cfg & (CHG_EN_SRC_BIT | CHG_EN_POLARITY_BIT);
		rc = regmap_update_bits(chip->regmap, chip->base + CHARGING_ENABLE_CMD,
					CHARGING_ENABLE_CMD_BIT, CHARGING_ENABLE_CMD_BIT);
		if (rc)
			goto rollback;
		rc = regmap_multi_reg_write(chip->regmap, seq, ARRAY_SIZE(seq));
		if (rc)
			goto rollback;
		sw_owned = true;
		rc = regmap_read(chip->regmap, chip->base + CHGR_CFG2, &cfg);
		if (rc || (cfg & CHG_EN_SRC_BIT)) {
			if (!rc)
				rc = -EIO;
			goto rollback;
		}
	}
	rc = regmap_update_bits(chip->regmap, chip->base + CHARGING_ENABLE_CMD,
				CHARGING_ENABLE_CMD_BIT, hold ? 0 : CHARGING_ENABLE_CMD_BIT);
	if (rc)
		goto rollback;
	rc = regmap_read(chip->regmap, chip->base + CHARGING_ENABLE_CMD,
			 &command);
	if (rc)
		goto rollback;
	if (!!(command & CHARGING_ENABLE_CMD_BIT) == hold) {
		rc = -EIO;
		goto rollback;
	}
	if (chip->charge_limit == 100 && sw_owned) {
		const struct reg_sequence seq[] = {
			{ chip->base + 0xd0, 0xa5 },
			{ chip->base + CHGR_CFG2,
			  (cfg & ~(CHG_EN_SRC_BIT | CHG_EN_POLARITY_BIT)) | hw_cfg },
		};

		rc = regmap_multi_reg_write(chip->regmap, seq, ARRAY_SIZE(seq));
		if (rc)
			goto rollback;
		rc = regmap_read(chip->regmap, chip->base + CHGR_CFG2, &cfg);
		if (rc || !(cfg & CHG_EN_SRC_BIT)) {
			if (!rc)
				rc = -EIO;
			goto rollback;
		}
		sw_owned = false;
	}
	chip->charge_hw_cfg = hw_cfg;
	chip->charge_sw_owned = sw_owned;
	if (chip->charge_held != hold) {
		chip->charge_held = hold;
		power_supply_changed(chip->chg_psy);
	}
	return 0;

rollback:
	restore_rc = smb_chiron_charge_restore(chip, initial_cfg, initial_command);
	chip->charge_hw_cfg = old_hw_cfg;
	chip->charge_sw_owned = old_sw_owned;
	chip->charge_held = old_held;
	if (restore_rc) {
		dev_err(chip->dev, "Charging state rollback failed: %d\n", restore_rc);
		if (!rc)
			rc = restore_rc;
	}
	return rc;
}

static int smb_chiron_limit_update(struct smb_chip *chip, bool online)
{
	struct power_supply *battery;
	union power_supply_propval capacity;
	bool hold = chip->charge_held;
	int rc;

	if (chip->charge_limit == 100 || !online)
		return smb_chiron_charge_hold(chip, false);
	battery = power_supply_get_by_name("chiron-battery");
	if (!battery)
		return smb_chiron_charge_hold(chip, true);
	rc = power_supply_get_property(battery, POWER_SUPPLY_PROP_CAPACITY, &capacity);
	power_supply_put(battery);
	/* Invalid SOC must not silently bypass an enabled charging limit. */
	if (rc || capacity.intval < 0 || capacity.intval > 100)
		hold = true;
	else if (capacity.intval >= 85)
		hold = true;
	else if (capacity.intval <= 83)
		hold = false;
	return smb_chiron_charge_hold(chip, hold);
}
