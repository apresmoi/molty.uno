// Package device holds the HTTP client and types for talking to a Molty device.
// The JSON shapes here mirror what the firmware (device/firmware/main/net/http.c) sends.
package device

// EyeState mirrors the full eye state JSON that the device sends on
// GET /api/eye/state and expects (as a partial) on POST /api/eye/state.
// All fields are optional on POST — the device merges the patch.
type EyeState struct {
	X         float64        `json:"x"`
	Y         float64        `json:"y"`
	PupilSize float64        `json:"pupil_size"`
	Blink     float64        `json:"blink"`
	Autoblink bool           `json:"autoblink"`
	Sclera    ScleraLayer    `json:"sclera"`
	Iris      IrisLayer      `json:"iris"`
	Limbal    LimbalLayer    `json:"limbal"`
	Pupil     PupilLayer     `json:"pupil"`
	Highlight HighlightLayer `json:"highlight"`
	Eyelid    EyelidLayer    `json:"eyelid"`
}

// ScleraLayer — background of the eye.
type ScleraLayer struct {
	Color string `json:"color"`
}

// IrisLayer — the coloured ring.
type IrisLayer struct {
	InnerColor string  `json:"inner_color"`
	OuterColor string  `json:"outer_color"`
	SpokeCount int     `json:"spoke_count"`
	SpokeAmp   float64 `json:"spoke_amp"`
	Radius     float64 `json:"radius"`
}

// LimbalLayer — thin ring between iris and sclera.
type LimbalLayer struct {
	Color     string  `json:"color"`
	Thickness float64 `json:"thickness"`
}

// PupilLayer — central dark disc.
type PupilLayer struct {
	Color string `json:"color"`
}

// HighlightLayer — specular catch-light.
type HighlightLayer struct {
	X     float64 `json:"x"`
	Y     float64 `json:"y"`
	R     float64 `json:"r"`
	Color string  `json:"color"`
}

// EyelidLayer — blink bar color.
type EyelidLayer struct {
	Color string `json:"color"`
}
