#pragma once

#include <obs.hpp>
#include <vector>
#include "util/threading.h"

enum class MultiviewLayout : uint8_t {
	HORIZONTAL_TOP_8_SCENES = 0,
	HORIZONTAL_BOTTOM_8_SCENES = 1,
	VERTICAL_LEFT_8_SCENES = 2,
	VERTICAL_RIGHT_8_SCENES = 3,
	HORIZONTAL_TOP_24_SCENES = 4,
	HORIZONTAL_TOP_18_SCENES = 5,
	SCENES_ONLY_4_SCENES = 6,
	SCENES_ONLY_9_SCENES = 7,
	SCENES_ONLY_16_SCENES = 8,
	SCENES_ONLY_25_SCENES = 9,
};
class Multiview {
public:
	Multiview();
	~Multiview();
	void Update(MultiviewLayout multiviewLayout, bool drawLabel,
		    bool drawSafeArea, bool drawAudioMeter, int selectedNewAudio);
	void Render(uint32_t cx, uint32_t cy);
	OBSSource GetSourceByPosition(int x, int y);
	// Volume printing
	float currentMagnitude[MAX_AUDIO_CHANNELS];

private:
	bool drawLabel, drawSafeArea, drawAudioMeter;
	double minimumLevel;
	int selectedAudio;
	int selectedTrack;
	size_t maxSrcs, numSrcs;
	MultiviewLayout multiviewLayout;
	gs_vertbuffer_t *actionSafeMargin = nullptr;
	gs_vertbuffer_t *graphicsSafeMargin = nullptr;
	gs_vertbuffer_t *fourByThreeSafeMargin = nullptr;
	gs_vertbuffer_t *leftLine = nullptr;
	gs_vertbuffer_t *topLine = nullptr;
	gs_vertbuffer_t *rightLine = nullptr;
	OBSVolMeter obs_volmeter;

	std::vector<OBSWeakSource> multiviewScenes;
	std::vector<OBSWeakSource> audioSource;
	std::vector<OBSSource> multiviewLabels;

	// Multiview position helpers
	float thickness = 4;
	float offset, thicknessx2 = thickness * 2, pvwprgCX, pvwprgCY, sourceX,
		      sourceY, labelX, labelY, scenesCX, scenesCY, ppiCX, ppiCY,
		      siX, siY, siCX, siCY, ppiScaleX, ppiScaleY, siScaleX,
		      siScaleY, fw, fh, ratio;

	// argb colors
	static const uint32_t outerColor = 0xFFD0D0D0;
	static const uint32_t labelColor = 0xD91F1F1F;
	static const uint32_t backgroundColor = 0xFF000000;
	static const uint32_t previewColor = 0xFF00D000;
	static const uint32_t programColor = 0xFFD00000;
	static const uint32_t backgroundNominalColor = 0xFF267F26; // Dark green
	static const uint32_t backgroundWarningColor =0xFF7F7F26; // Dark yellow
	static const uint32_t backgroundErrorColor = 0xFF7F2626; // Dark red
	static const uint32_t foregroundNominalColor = 0xFF4CFF4C; // Bright green
	static const uint32_t foregroundWarningColor = 0xFFFFFF4C; // Bright yellow
	static const uint32_t foregroundErrorColor = 0xFFFF4C4C; // Bright red

	// Volume printing
	static void OBSVolumeLevelChanged(void *data,
			      const float magnitude[MAX_AUDIO_CHANNELS],
			      const float peak[MAX_AUDIO_CHANNELS],
			      const float inputPeak[MAX_AUDIO_CHANNELS]);
	static void OBSOutputVolumeLevelChanged(void *param, size_t mix_idx,
						struct audio_data *data);
	void setLevels(const float magnitude[MAX_AUDIO_CHANNELS]);
	inline int convertToInt(float number);
	void InitAudioMeter();
	void RenderAudioMeter();
	void ConnectAudioOutput();
	void DrawScale(int indexFromLast, float xCoordinate, float yCoordinate);
	float round(float var);
};

static inline void startRegion(int vX, int vY, int vCX, int vCY, float oL,
			       float oR, float oT, float oB)
{
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(vX, vY, vCX, vCY);
	gs_ortho(oL, oR, oT, oB, -100.0f, 100.0f);
}

static inline void endRegion()
{
	gs_viewport_pop();
	gs_projection_pop();
}
