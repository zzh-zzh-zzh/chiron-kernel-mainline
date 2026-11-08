/* Derived from TI/Xiaomi GPL-2.0 tas2559-regmap.c. */
static void irq_work_routine(struct work_struct *work)
{
	struct tas2559_priv *pTAS2559 =
		container_of(work, struct tas2559_priv, irq_work.work);
	struct TConfiguration *pConfiguration;
	unsigned int nDevLInt1Status = 0, nDevLInt2Status = 0;
	unsigned int nDevRInt1Status = 0, nDevRInt2Status = 0;
	int nCounter = 2;
	int nResult = 0;

	mutex_lock(&pTAS2559->codec_lock);


	if (pTAS2559->stopping || pTAS2559->faulted || pTAS2559->mbRuntimeSuspend) {
		dev_info(pTAS2559->dev, "%s, Runtime Suspended\n", __func__);
		goto end;
	}

	if (pTAS2559->mnErrCode & ERROR_FAILSAFE)
		goto program;

	if (!pTAS2559->mbPowerUp) {
		dev_info(pTAS2559->dev, "%s, device not powered\n", __func__);
		goto end;
	}

	if ((!pTAS2559->mpFirmware->mnConfigurations)
	    || (!pTAS2559->mpFirmware->mnPrograms)) {
		dev_info(pTAS2559->dev, "%s, firmware not loaded\n", __func__);
		goto end;
	}

	pConfiguration = &(pTAS2559->mpFirmware->mpConfigurations[pTAS2559->mnCurrentConfiguration]);

	if (pConfiguration->mnDevices & DevA) {
		nResult = tas2559_dev_read(pTAS2559, DevA, TAS2559_FLAGS_1, &nDevLInt1Status);

		if (nResult < 0)
			goto program;
		nResult = tas2559_dev_read(pTAS2559, DevA, TAS2559_FLAGS_2, &nDevLInt2Status);
		if (nResult < 0)
			goto program;

		if (((nDevLInt1Status & 0xfc) != 0) || ((nDevLInt2Status & 0x0c) != 0)) {
			/* in case of INT_OC, INT_UV, INT_OT, INT_BO, INT_CL, INT_CLK1, INT_CLK2 */
			dev_dbg(pTAS2559->dev, "IRQ critical Error DevA: 0x%x, 0x%x\n",
				nDevLInt1Status, nDevLInt2Status);

			if (nDevLInt1Status & 0x80) {
				pTAS2559->mnErrCode |= ERROR_OVER_CURRENT;
				dev_err(pTAS2559->dev, "DEVA SPK over current!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_OVER_CURRENT;

			if (nDevLInt1Status & 0x40) {
				pTAS2559->mnErrCode |= ERROR_UNDER_VOLTAGE;
				dev_err(pTAS2559->dev, "DEVA SPK under voltage!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_UNDER_VOLTAGE;

			if (nDevLInt1Status & 0x20) {
				pTAS2559->mnErrCode |= ERROR_CLK_HALT;
				dev_err(pTAS2559->dev, "DEVA clk halted!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_CLK_HALT;

			if (nDevLInt1Status & 0x10) {
				pTAS2559->mnErrCode |= ERROR_DIE_OVERTEMP;
				dev_err(pTAS2559->dev, "DEVA die over temperature!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_DIE_OVERTEMP;

			if (nDevLInt1Status & 0x08) {
				pTAS2559->mnErrCode |= ERROR_BROWNOUT;
				dev_err(pTAS2559->dev, "DEVA brownout!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_BROWNOUT;

			if (nDevLInt1Status & 0x04) {
				pTAS2559->mnErrCode |= ERROR_CLK_LOST;
				dev_err(pTAS2559->dev, "DEVA clock lost!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_CLK_LOST;

			if (nDevLInt2Status & 0x08) {
				pTAS2559->mnErrCode |= ERROR_CLK_DET1;
				dev_err(pTAS2559->dev, "DEVA clk detection 1!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_CLK_DET1;

			if (nDevLInt2Status & 0x04) {
				pTAS2559->mnErrCode |= ERROR_CLK_DET2;
				dev_err(pTAS2559->dev, "DEVA clk detection 2!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_CLK_DET2;

			goto program;
		} else {
			dev_dbg(pTAS2559->dev, "IRQ status DevA: 0x%x, 0x%x\n",
				nDevLInt1Status, nDevLInt2Status);
			nCounter = 2;

			while (nCounter > 0) {
				nResult = tas2559_dev_read(pTAS2559, DevA, TAS2559_POWER_UP_FLAG_REG, &nDevLInt1Status);

				if (nResult < 0)
					goto program;

				if ((nDevLInt1Status & 0xc0) == 0xc0)
					break;

				nCounter--;

				if (nCounter > 0) {
					/* in case check pow status just after power on TAS2559 */
					dev_dbg(pTAS2559->dev, "PowSts A: 0x%x, check again after 10ms\n",
						nDevLInt1Status);
					msleep(10);
				}
			}

			if ((nDevLInt1Status & 0xc0) != 0xc0) {
				dev_err(pTAS2559->dev, "%s, Critical DevA ERROR B[%d]_P[%d]_R[%d]= 0x%x\n",
					__func__,
					TAS2559_BOOK_ID(TAS2559_POWER_UP_FLAG_REG),
					TAS2559_PAGE_ID(TAS2559_POWER_UP_FLAG_REG),
					TAS2559_PAGE_REG(TAS2559_POWER_UP_FLAG_REG),
					nDevLInt1Status);
				pTAS2559->mnErrCode |= ERROR_CLASSD_PWR;
				goto program;
			}

			pTAS2559->mnErrCode &= ~ERROR_CLASSD_PWR;
		}
	}

	if (pConfiguration->mnDevices & DevB) {
		nResult = tas2559_dev_read(pTAS2559, DevB, TAS2560_FLAGS_1, &nDevRInt1Status);

		if (nResult < 0)
			goto program;
		nResult = tas2559_dev_read(pTAS2559, DevB, TAS2560_FLAGS_2, &nDevRInt2Status);
		if (nResult < 0)
			goto program;

		if (((nDevRInt1Status & 0xfc) != 0) || ((nDevRInt2Status & 0xc0) != 0)) {
			/* in case of INT_OC, INT_UV, INT_OT, INT_BO, INT_CL, INT_CLK1, INT_CLK2 */
			dev_dbg(pTAS2559->dev, "IRQ critical Error DevB: 0x%x, 0x%x\n",
				nDevRInt1Status, nDevRInt2Status);

			if (nDevRInt1Status & 0x80) {
				pTAS2559->mnErrCode |= ERROR_OVER_CURRENT;
				dev_err(pTAS2559->dev, "DEVB SPK over current!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_OVER_CURRENT;

			if (nDevRInt1Status & 0x40) {
				pTAS2559->mnErrCode |= ERROR_UNDER_VOLTAGE;
				dev_err(pTAS2559->dev, "DEVB SPK under voltage!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_UNDER_VOLTAGE;

			if (nDevRInt1Status & 0x20) {
				pTAS2559->mnErrCode |= ERROR_CLK_HALT;
				dev_err(pTAS2559->dev, "DEVB clk halted!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_CLK_HALT;

			if (nDevRInt1Status & 0x10) {
				pTAS2559->mnErrCode |= ERROR_DIE_OVERTEMP;
				dev_err(pTAS2559->dev, "DEVB die over temperature!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_DIE_OVERTEMP;

			if (nDevRInt1Status & 0x08) {
				pTAS2559->mnErrCode |= ERROR_BROWNOUT;
				dev_err(pTAS2559->dev, "DEVB brownout!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_BROWNOUT;

			if (nDevRInt1Status & 0x04) {
				pTAS2559->mnErrCode |= ERROR_CLK_LOST;
				dev_err(pTAS2559->dev, "DEVB clock lost!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_CLK_LOST;

			if (nDevRInt2Status & 0x80) {
				pTAS2559->mnErrCode |= ERROR_CLK_DET1;
				dev_err(pTAS2559->dev, "DEVB clk detection 1!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_CLK_DET1;

			if (nDevRInt2Status & 0x40) {
				pTAS2559->mnErrCode |= ERROR_CLK_DET2;
				dev_err(pTAS2559->dev, "DEVB clk detection 2!\n");
			} else
				pTAS2559->mnErrCode &= ~ERROR_CLK_DET2;

			goto program;
		} else {
			dev_dbg(pTAS2559->dev, "IRQ status DevB: 0x%x, 0x%x\n",
				nDevRInt1Status, nDevRInt2Status);
			nCounter = 2;

			while (nCounter > 0) {
				nResult = tas2559_dev_read(pTAS2559, DevB, TAS2560_POWER_UP_FLAG_REG, &nDevRInt1Status);

				if (nResult < 0)
					goto program;

				if ((nDevRInt1Status & 0xc0) == 0xc0)
					break;

				nCounter--;

				if (nCounter > 0) {
					/* in case check pow status just after power on TAS2560 */
					dev_dbg(pTAS2559->dev, "PowSts B: 0x%x, check again after 10ms\n",
						nDevRInt1Status);
					msleep(10);
				}
			}

			if ((nDevRInt1Status & 0xc0) != 0xc0) {
				dev_err(pTAS2559->dev, "%s, Critical DevB ERROR B[%d]_P[%d]_R[%d]= 0x%x\n",
					__func__,
					TAS2559_BOOK_ID(TAS2560_POWER_UP_FLAG_REG),
					TAS2559_PAGE_ID(TAS2560_POWER_UP_FLAG_REG),
					TAS2559_PAGE_REG(TAS2560_POWER_UP_FLAG_REG),
					nDevRInt1Status);
				pTAS2559->mnErrCode |= ERROR_CLASSD_PWR;
				goto program;
			}

			pTAS2559->mnErrCode &= ~ERROR_CLASSD_PWR;
		}
	}

	goto end;

program:
	/* hardware reset and reload */
	tas2559_force_off(pTAS2559);

end:


	if (nResult < 0)
		tas2559_force_off(pTAS2559);
	if (!pTAS2559->stopping && !pTAS2559->faulted && pTAS2559->mbPowerUp)
		tas2559_enableIRQ(pTAS2559, DevBoth, true);
	mutex_unlock(&pTAS2559->codec_lock);
}

static void timer_work_routine(struct work_struct *work)
{
	struct tas2559_priv *pTAS2559 = container_of(work, struct tas2559_priv, mtimerwork);
	int nResult = 0, nTemp, nActTemp;
	struct TProgram *pProgram;
	int *avg = &pTAS2559->temperature_sum;

	mutex_lock(&pTAS2559->codec_lock);


	if (pTAS2559->stopping || pTAS2559->faulted || pTAS2559->mbRuntimeSuspend) {
		dev_info(pTAS2559->dev, "%s, Runtime Suspended\n", __func__);
		goto end;
	}

	if (!pTAS2559->mpFirmware->mnConfigurations) {
		dev_info(pTAS2559->dev, "%s, firmware not loaded\n", __func__);
		goto end;
	}

	pProgram = &(pTAS2559->mpFirmware->mpPrograms[pTAS2559->mnCurrentProgram]);

	if (!pTAS2559->mbPowerUp
	    || (pProgram->mnAppMode != TAS2559_APP_TUNINGMODE)) {
		dev_info(pTAS2559->dev, "%s, pass, Pow=%d, program=%s\n",
			 __func__, pTAS2559->mbPowerUp, pProgram->mpName);
		goto end;
	}

	nResult = tas2559_get_die_temperature(pTAS2559, &nTemp);

	if (nResult >= 0) {
		nActTemp = (int)(nTemp >> 23);
		dev_dbg(pTAS2559->dev, "Die=0x%x, degree=%d\n", nTemp, nActTemp);

		if (!pTAS2559->mnDieTvReadCounter)
			(*avg) = 0;

		pTAS2559->mnDieTvReadCounter++;
		(*avg) += nActTemp;

		if (!(pTAS2559->mnDieTvReadCounter % LOW_TEMPERATURE_COUNTER)) {
			(*avg) /= LOW_TEMPERATURE_COUNTER;
			dev_dbg(pTAS2559->dev, "check : avg=%d\n", (*avg));

			if (((*avg) & 0x80000000) != 0) {
				/* if Die temperature is below ZERO */
				if (pTAS2559->mnDevCurrentGain != LOW_TEMPERATURE_GAIN) {
					nResult = tas2559_set_DAC_gain(pTAS2559, DevBoth, LOW_TEMPERATURE_GAIN);

					if (nResult < 0)
						goto end;

					pTAS2559->mnDevCurrentGain = LOW_TEMPERATURE_GAIN;
					dev_dbg(pTAS2559->dev, "LOW Temp: set gain to %d\n", LOW_TEMPERATURE_GAIN);
				}
			} else if ((*avg) > 5) {
				/* if Die temperature is above 5 degree C */
				if (pTAS2559->mnDevCurrentGain != pTAS2559->mnDevGain) {
					nResult = tas2559_set_DAC_gain(pTAS2559, DevBoth, pTAS2559->mnDevGain);

				if (nResult < 0)
					goto end;

				pTAS2559->mnDevCurrentGain = pTAS2559->mnDevGain;
				dev_dbg(pTAS2559->dev, "LOW Temp: set gain to original\n");
				}
			}

			(*avg) = 0;
		}

		if (pTAS2559->mbPowerUp)
			hrtimer_start(&pTAS2559->mtimer,
				      ns_to_ktime((u64)LOW_TEMPERATURE_CHECK_PERIOD * NSEC_PER_MSEC), HRTIMER_MODE_REL);
	}

end:


	if (nResult < 0)
		tas2559_force_off(pTAS2559);
	if (!pTAS2559->stopping && !pTAS2559->faulted && pTAS2559->mbPowerUp)
		tas2559_enableIRQ(pTAS2559, DevBoth, true);
	mutex_unlock(&pTAS2559->codec_lock);
}

static enum hrtimer_restart temperature_timer_func(struct hrtimer *timer)
{
	struct tas2559_priv *pTAS2559 = container_of(timer, struct tas2559_priv, mtimer);

	if (pTAS2559->mbPowerUp && !pTAS2559->stopping && !pTAS2559->faulted) {
		schedule_work(&pTAS2559->mtimerwork);

		if (!delayed_work_pending(&pTAS2559->irq_work))
			schedule_delayed_work(&pTAS2559->irq_work, msecs_to_jiffies(20));
	}

	return HRTIMER_NORESTART;
}

