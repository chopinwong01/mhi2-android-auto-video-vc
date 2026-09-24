# Design Decisions and Lessons

This is a decision record, not a claim that every branch has been road-proven.
It summarizes the constraints that should shape future changes.

## Keep the center renderer out of the secondary path

**Decision:** use `GAL_DUALSCREEN_OUTPUT=withhold` and send the secondary
elementary stream to `stream-player`.

**Reason:** stock GAL has one shared `CVideoRenderer`. Forwarding secondary
`playbackStart` to it can reconfigure the primary path and blank the cente
display. This is an ownership issue, not merely a performance problem.

**Implication:** do not “fix” a black cluster by switching to output mode `gal`
without first accepting that the center-display regression is expected.

**Alternative considered:** create or redirect a second GAL-private renderer.

**Why it is not the deployment default:** it depends on a private object model
and renderer lifecycle that are much harder to verify than the external-process
boundary. Historical `own` variants remain research paths, not evidence that a
second private renderer is safe on this firmware.

## Separate the four state machines

The project has at least four independently observable states:

| Layer | Question it answers |
|---|---|
| AAP/GAL protocol | Did the phone open and feed the secondary service? |
| Stream/player | Did an H.264 decoder receive valid bootstrap data and frames? |
| DMDT/MOST | Is the Cockpit pointed at the intended displayable/context? |
| Java HMI | Does the center UI believe Android Auto has entered or left its canvas? |

A successful event at one layer does not prove the next. This is why a DMDT
command cannot by itself implement Android Auto Exit, and why a registered sink
does not prove a visible Cockpit frame.

**Decision:** document every experiment by the state machine it changes and
the evidence it is expected to produce.

**Reason:** protocol, decoding, routing, and Java HMI failures frequently have
the same outward symptom: a black screen or an unexpected UI transition.

**Implication:** do not group an AAP metadata change, a UiConfig change, a
player change, and a JAR change into one deployable test package. That might
produce a demonstration, but it cannot produce a diagnosis.

## Fail closed on ABI uncertainty

**Decision:** verify the P4521 controller/vtable symbols before constructing
the secondary endpoint; leave stock GAL untouched if they do not match.

**Reason:** the hook relies on a fixed-address ARM C++ ABI, including a
multiple-inheritance callback-handler layout. A close-looking firmware or a
single plausible offset is not a safe compatibility signal.

**Implication:** a port to another train begins with binary inspection and new
runtime guards. It does not begin by editing constants until the preload loads.

## Copy the compatible shape, not a guessed object

**Decision:** observe the primary sink's configuration/registration sequence,
then construct a separate secondary sink and callback handler using the
target's constructor.

**Reason:** an early type/offset assumption confused callback-handler and
implementation identities. The target's constructor plus a vtable check gives
the secondary object the same ABI family without rewriting the primary path.

**Implication:** retain independent allocations and pointer/vtable logging.
Do not deduplicate them into a clever shared object merely because the fields
appear similar in a decompiler.

## TCP Loopback is the Definitive Video Transport

**Decision:** Use TCP loopback (`tcp://127.0.0.1:12346` with 2 MB buffers) for video transport.

**Reason:**
1. **Immutable RTOS Buffer Limit on QNX 6.5.0:** Measured on-car with `vc_sockbuf`, QNX 6.5.0 SP1 hardcodes `AF_UNIX` stream sockets to **7,168 bytes send / 5,120 bytes receive**, and `setsockopt(SO_SNDBUF/SO_RCVBUF)` is completely ignored. Transmitting 28 KB (p95) to 140 KB (IDR keyframes) H.264 video frames over `AF_UNIX` requires 6 to 27 round trips per frame through the 5 KB window, thrashing the scheduler and collapsing video throughput to **3.3–4 FPS**.
2. **Full 2 MB Socket Buffering on TCP:** TCP loopback (`AF_INET`) accepts full 2 MB socket buffers (`setsockopt(SO_SNDBUF/SO_RCVBUF, 2097152)`), allowing even 140 KB keyframes to cross in a single atomic write at **25–30 FPS**.
3. **No Context-Switch Advantage for AF_UNIX:** On QNX 6.5.0, both `AF_UNIX` and `AF_INET` are handled by the same `io-pkt` network manager daemon.
4. **Filesystem Destructor Safety:** A pathname-backed UNIX-domain socket (`/tmp/gal_video.sock`) can be accidentally unlinked if an un-guarded child process executes shared library destructors on exit. TCP has no filesystem path to unlink.

**Implication:** TCP loopback is not a temporary fallback; it is the definitive, high-throughput production transport on Harman MIB2 QNX 6.5.0.


## Treat decoder bootstrap as a protocol contract

**Decision:** cache codec configuration and one bounded IDR, replaying them to
a newly connected player before live delta frames; clear all of it on a new
phone playback session.

**Reason:** a late decoder cannot begin with a predicted frame, and an old IDR
must never be replayed into a new encoder session.

**Implication:** “the player connected” is not enough. Logs must establish
codec configuration, a current keyframe, and the first decodable frame. An
oversize keyframe is rejected from the bounded cache rather than making the
hook retain arbitrary video memory.

## Exact geometry bytes are part of the test case

**Decision:** support an exact UiConfig protobuf override.

**Reason:** UI safe-area behavior depends on nested UiConfig fields that have
different effects: field 1 changes encoded content/crop, field 2 supplies
`contentInsets`, field 3 supplies `stableInsets`, and field 4 is UI theme.

**Implication:** scalar inset values do nothing while a hex override is present.
Record the byte string with every visual result, and never relabel a field based
on a single visual correlation.

## ACK feedback is deliberately fail-open

**Decision:** defer phone ACKs to player presentation while feedback is healthy,
but resume immediate ACKs if the player has not connected or becomes silent fo
more than 500 ms.

**Reason:** waiting indefinitely turns a player failure into a phone-session
failure. The fallback preserves Android Auto connectivity at the cost of losing
strict render pacing during that fault.

**Implication:** a live phone session does not prove healthy player feedback.
Check `event=ack.client`, ACK timeout/fallback events, and player evidence.

**Alternative considered:** always ACK immediately, or never ACK without a
render confirmation.

**Why neither extreme is used:** immediate ACK sacrifices backpressure during
normal operation; infinite waiting turns a player failure into an Android Auto
session failure. The current 500 ms fallback is a fault-containment choice,
not a claim that the rendered frame was observed in that fallback interval.

## DMDT accepts some invalid-looking requests silently

**Decision:** document display ID 4 explicitly and restore displayable 33 on
teardown.

**Reason:** the Virtual Cockpit's DMDT display ID is 4, while `dmdt gs` exposes
a different printed index. A command using the wrong value can appear to
succeed while changing nothing.

**Implication:** a shell return code is insufficient; inspect `dmdt gs` and the
physical display.

## Make OEM restoration a first-class requirement

**Decision:** restore Displayable 33 in context 70 on player teardown, with an
emergency restoration attempt after forced player termination.

**Reason:** presentation, ignition, and reverse-camera transitions can disrupt
the custom route independently of the Android Auto session. A frozen or stale
custom display is worse than losing the experimental image.

**Implication:** every acceptance run includes normal teardown, forced-process
recovery where safe, and a reboot/ignition observation. A happy-path screenshot
does not validate lifecycle behavior.

## Preserve a narrow experiment boundary

**Decision:** use file configuration, bisect switches, and one-variable tests.

**Reason:** the supervisor's environment budget is small, and discovery can
depend on several coupled metadata changes. Changing protocol version, service
registration, display metadata, focus, and geometry together destroys causal
evidence.

**Implication:** default settings should remain boring. Experimental switches
need a stated hypothesis, expected evidence, and a rollback point.

## Keep DHU evidence in its lane

**Decision:** use DHU/service-discovery captures for protocol shape and
subtractive experiments, but label them as offline evidence.

**Reason:** DHU cannot validate the target GAL ABI, QNX process environment,
MOST route, Cockpit power behavior, or Java HMI state machine.

**Implication:** a DHU result informs the next car test; it cannot replace it.
Preserve the exact DHU version/configuration and the specific narrowed
question, then repeat the final candidate on the target.

## Keep Java companion work separate

**Decision:** document and deploy J9/ASL companion changes as a separate,
high-risk layer rather than as part of the native hook package.

**Reason:** the Java HMI controls center-canvas policy but does not carry the
secondary H.264 path. A modern-host-valid replacement class can fail target J9
verification and affect the entire center UI.

**Implication:** retain a known-good JAR and an independent rollback route;
build with the established legacy-compatible toolchain; and test boot, Exit,
re-entry, Disconnect, and reboot separately from native stream changes.

## Keep exact geometry bytes with the result

**Decision:** retain both readable inset/theme values and an exact UiConfig
payload override in the test record.

**Reason:** outer margins, field-1 crop, field-2 contentInsets, field-3
stableInsets, and field-4 theme are different inputs with visually confusable
results. An exact override also takes precedence over scalar settings.

**Implication:** a geometry screenshot without the emitted bytes is incomplete
evidence. Update or remove the hex override deliberately for every field test.

## Treat LLM assistance as an untrusted hypothesis generato

**Decision:** use LLMs for navigation, drafting, code review, and generating
test hypotheses, but never treat their explanation, decompilation narrative,
or generated patch as evidence that a target behavior is true.

**Reason:** current LLMs can be verbose yet incomplete, internally illogical,
or overconfident to the extent of calling an answer “absolutely” correct while
being wrong about vital details: C++ object identity, an ABI offset, wire-field
meaning, process ownership, shell behavior, or the difference between a DHU
observation and a car result. In this project, attractive early claims about
object layout, rendering ownership, and HMI/focus behavior needed correction
through source inspection and target evidence. An error at this level can break
Android Auto, lose the center UI, or leave a display route in the wrong state.

**Implication:** every AI-suggested change needs all of the following before
deployment:

1. A human-readable hypothesis and a single intended variable.
2. A source/binary inspection that supports each target-specific claim.
3. A reversible package and an explicit rollback path.
4. Target logs and physical observation appropriate to the claim.
5. A written correction when the result disproves the initial explanation.

LLMs are credited as tools in this project, not listed as maintainers or GitHub
contributors. The maintainer is responsible for the code, the safety boundary,
and every published assertion.
