
/* VoIP pcm write in celadon devices goes to bt alsa card */
int out_write_bt (struct stream_out *out, struct audio_device *adev, const void* buffer,
                         size_t bytes)
{
        int ret = 0;
        size_t frames_in = round_to_16_mult(out->pcm_config->period_size);
        size_t frames_out = round_to_16_mult(bt_out_config.period_size);
        size_t buf_size_out = bt_out_config.channels * frames_out * SAMPLE_SIZE_IN_BYTES;
        size_t buf_size_in = out->pcm_config->channels * frames_in * SAMPLE_SIZE_IN_BYTES;
        size_t buf_size_remapped = bt_out_config.channels * frames_in * SAMPLE_SIZE_IN_BYTES;
        int16_t *buf_out = (int16_t *) malloc (buf_size_out);
        int16_t *buf_in = (int16_t *) malloc (buf_size_in);
        int16_t *buf_remapped = (int16_t *) malloc (buf_size_remapped);

        if(adev->voip_out_resampler == NULL) {
            int ret = create_resampler(out->pcm_config->rate /*src rate*/, bt_out_config.rate /*dst rate*/, bt_out_config.channels/*dst channels*/,
                            RESAMPLER_QUALITY_DEFAULT, NULL, &(adev->voip_out_resampler));
            ALOGD("%s : frames_in %zu frames_out %zu",__func__, frames_in, frames_out);
            ALOGD("%s : to write bytes : %zu", __func__, bytes);
            ALOGD("%s : size_in %zu size_out %zu size_remapped %zu", __func__, buf_size_in, buf_size_out, buf_size_remapped);

            if (ret != 0) {
                adev->voip_out_resampler = NULL;
                ALOGE("%s : Failure to create resampler %d", __func__, ret);

                free(buf_in);
                free(buf_out);
                free(buf_remapped);
            } else {
                ALOGD("%s : voip_out_resampler created rate : [%d -> %d]", __func__, out->pcm_config->rate, bt_out_config.rate);
            }
        }

        memset(buf_in, 0, buf_size_in);
        memset(buf_remapped, 0, buf_size_remapped);
        memset(buf_out, 0, buf_size_out);

        memcpy_s(buf_in,buf_size_in, buffer, buf_size_in);

#ifdef DEBUG_PCM_DUMP
        if(sco_call_write != NULL) {
            fwrite(buf_in, 1, buf_size_in, sco_call_write);
        } else {
            ALOGD("%s : sco_call_write was NULL, no dump", __func__);
        }
#endif

        adjust_channels(buf_in, out->pcm_config->channels, buf_remapped, bt_out_config.channels,
                                        SAMPLE_SIZE_IN_BYTES, buf_size_in);

        //ALOGV("remapping : [%d -> %d]", out->pcm_config->channels, bt_out_config.channels);

#ifdef DEBUG_PCM_DUMP
        if(sco_call_write_remapped != NULL) {
            fwrite(buf_remapped, 1, buf_size_remapped, sco_call_write_remapped);
        } else {
            ALOGD("%s : sco_call_write_remapped was NULL, no dump", __func__);
        }
#endif

        if(adev->voip_out_resampler != NULL) {
            adev->voip_out_resampler->resample_from_input(adev->voip_out_resampler, (int16_t *)buf_remapped, (size_t *)&frames_in, (int16_t *) buf_out, (size_t *)&frames_out);
            //ALOGV("%s : upsampling [%d -> %d]",__func__, out->pcm_config->rate, bt_out_config.rate);
        }

        ALOGV("%s : modified frames_in %zu frames_out %zu",__func__, frames_in, frames_out);

        buf_size_out = bt_out_config.channels * frames_out * SAMPLE_SIZE_IN_BYTES;
        bytes = out->pcm_config->channels * frames_in * SAMPLE_SIZE_IN_BYTES;

#ifdef DEBUG_PCM_DUMP
        if(sco_call_write_bt != NULL) {
            fwrite(buf_out, 1, buf_size_out, sco_call_write_bt);
        } else {
            ALOGD("%s : sco_call_write was NULL, no dump", __func__);
        }
#endif

        ret = pcm_write(out->pcm, buf_out, buf_size_out);

        free(buf_in);
        free(buf_out);
        free(buf_remapped);

return ret;
}

/* VoIP pcm read from bt alsa card */
int in_read_bt (struct stream_in *in, struct audio_device *adev, void* buffer,
		size_t bytes)
{
	int ret = 0;
        size_t frames_out = round_to_16_mult(in->pcm_config->period_size);
        size_t frames_in = round_to_16_mult(bt_in_config.period_size);
        size_t buf_size_out = in->pcm_config->channels * frames_out * SAMPLE_SIZE_IN_BYTES;
        size_t buf_size_in = bt_in_config.channels * frames_in * SAMPLE_SIZE_IN_BYTES;
        size_t buf_size_remapped = in->pcm_config->channels * frames_in * SAMPLE_SIZE_IN_BYTES;
        int16_t *buf_out = (int16_t *) malloc (buf_size_out);
        int16_t *buf_in = (int16_t *) malloc (buf_size_in);
        int16_t *buf_remapped = (int16_t *) malloc (buf_size_remapped);

        if(adev->voip_in_resampler == NULL) {
            int ret = create_resampler(bt_in_config.rate /*src rate*/, in->pcm_config->rate /*dst rate*/, in->pcm_config->channels/*dst channels*/,
                        RESAMPLER_QUALITY_DEFAULT, NULL, &(adev->voip_in_resampler));
            ALOGV("%s : bytes_requested : %zu", __func__, bytes);
            ALOGV("%s : frames_in %zu frames_out %zu",__func__, frames_in, frames_out);
            ALOGD("%s : size_in %zu size_out %zu size_remapped %zu", __func__, buf_size_in, buf_size_out, buf_size_remapped);
            if (ret != 0) {
                adev->voip_in_resampler = NULL;
                ALOGE("%s : Failure to create resampler %d", __func__, ret);

                free(buf_in);
                free(buf_out);
                free(buf_remapped);
            } else {
                ALOGD("%s : voip_in_resampler created rate : [%d -> %d]", __func__, bt_in_config.rate, in->pcm_config->rate);
            }
        }

        memset(buf_in, 0, buf_size_in);
        memset(buf_remapped, 0, buf_size_remapped);
        memset(buf_out, 0, buf_size_out);

        ret = pcm_read(in->pcm, buf_in, buf_size_in);

#ifdef DEBUG_PCM_DUMP
        if(sco_call_read != NULL) {
            fwrite(buf_in, 1, buf_size_in, sco_call_read);
        } else {
            ALOGD("%s : sco_call_read was NULL, no dump", __func__);
        }
#endif
        adjust_channels(buf_in, bt_in_config.channels, buf_remapped, in->pcm_config->channels,
                                        SAMPLE_SIZE_IN_BYTES, buf_size_in);

        //ALOGV("%s : remapping : [%d -> %d]", __func__, bt_in_config.channels, in->pcm_config->channels);

#ifdef DEBUG_PCM_DUMP
        if(sco_call_read_remapped != NULL) {
            fwrite(buf_remapped, 1, buf_size_remapped, sco_call_read_remapped);
        } else {
            ALOGD("%s : sco_call_read_remapped was NULL, no dump", __func__);
        }
#endif

        if(adev->voip_in_resampler != NULL) {
            adev->voip_in_resampler->resample_from_input(adev->voip_in_resampler, (int16_t *)buf_remapped, (size_t *)&frames_in, (int16_t *) buf_out, (size_t *)&frames_out);
            //ALOGV("%s : upsampling [%d -> %d]",__func__, bt_in_config.rate, in->pcm_config->rate);
        }

        ALOGV("%s : modified frames_in %zu frames_out %zu",__func__, frames_in, frames_out);

        buf_size_out = in->pcm_config->channels * frames_out * SAMPLE_SIZE_IN_BYTES;
        bytes = buf_size_out;

#ifdef DEBUG_PCM_DUMP
        if(sco_call_read_bt != NULL) {
            fwrite(buf_out, 1, buf_size_out, sco_call_read_bt);
        } else {
            ALOGD("%s : sco_call_read_bt was NULL, no dump", __func__);
        }
#endif

        memcpy_s(buffer,buf_size_out, buf_out, buf_size_out);

        free(buf_in);
        free(buf_out);
        free(buf_remapped);

return ret;
}
