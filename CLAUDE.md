# ST EQ — guide for agents

ST EQ is a 24-band parametric / dynamic EQ in the ST family, built on the shared module in `../st-shell`
(https://github.com/davidstokarski/st-shell). Read the shell docs first:

- `../st-shell/CLAUDE.md` — non-negotiables (design, real-time safety, native menus, verification)
- `../st-shell/docs/DESIGN.md`, `../st-shell/docs/ARCHITECTURE.md`, `../st-shell/docs/EXAMPLES.md`
- `../st-sat` is the reference plugin; this one follows its structure.

Plugin-specific notes:

- Parameters: `Source/Params.*`. IDs are permanent. 24 fixed band slots `b<n>_<suffix>` with a `used` flag;
  adding / removing a band = setting `used` (undoable, see `StEqProcessor::addBand / removeBand`).
- Filter maths lives in the shell (`st::dsp::eq`, analog prototypes + analog-matched biquads, `EqCascade`).
- DSP: `Source/dsp/Engine.*` — Zero Latency (IIR), Natural Phase (IIR + short correction FIR), Linear Phase
  (FIR of the analog magnitude, dynamic bands as IIR deltas). FIRs and auto gain are computed off the audio
  thread by `Source/dsp/FirWorker.*` and swapped lock-free into `st::dsp::PartitionedConvolver`.
  Mode / resolution changes re-prepare on the message thread inside `suspendProcessing` (`applyModeChange`).
- Spectral dynamics: `Source/dsp/Spectral.*` — one STFT stage (shell `st::dsp::StftProcessor`) after the main EQ,
  applying each spectral band's dynamic delta per bin. It only exists while a band has `spectral` on
  (`StEqProcessor::anySpectralBand`), so toggling it re-prepares through `applyModeChange` (latency changes).
  UI reads spectral meters via `StEqProcessor::bandMeter`.
- Instances: `Source/ui/Instances.*` — `InstanceHub` (other ST EQs via `st::InstanceRegistry`, their spectra through
  the analyser history tap, collisions) and `EqStrip` (ST EQ's module in the shell's `st::MultiView`: a compact,
  fully editable `EqDisplay (proc, true)` + bypass switch). The multiview (all ST plug-ins, by track) is the shell's;
  the editor opens it full-window. Instances can disappear at any time: the registry removes them synchronously.
  The tests load the installed ST Sat AU in-process to check the cross-binary multiview.
- EQ Sketch / EQ Match share the band fitter in `Source/dsp/Sketch.*`.
- UI: `Source/ui/EqDisplay.*` (display + all band interaction), `Source/ui/BandIsland.*` (the floating band
  controls; dynamics expand inline), `Source/ui/Panels.*` (EQ Match and output popovers, bottom bar), `Source/PluginEditor.*`.
  The look follows Pro-Q 4's band controls and display closely (measured from FabFilter's help screenshots);
  keep it that way, without FabFilter names or logos. ST EQ uses a neutral graphite theme (set in the processor
  constructor) instead of ST Sat's red display.
- Verify: `StEqPluginTests` (responses per mode vs analog, latency, dynamics, stress, state, CPU, UI paint cost),
  `StEqPluginTests --snapshot <dir>` (look at the PNGs), `auval -v aufx Steq StAu`.
- Release: `scripts/release.sh`.
