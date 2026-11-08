#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Geometry.hpp"
#include "Identification.hpp"

// The state of the game resources RR reads at the evaluation point, without D3D12. It is not fixed in the
// code: the registry learns it from the game's barriers in submission order and confirms it at every
// ExecuteCommandLists.
namespace rr::core {
// What RR reads at the TAA draw, in the order of ReadStates.
enum class Read : std::uint8_t {
    T,
    Velocity,
    Albedo,
    Normals,
    DepthPlane0,
    DepthPlane1,
};

inline constexpr std::size_t READ_COUNT = static_cast<std::size_t>(Read::DepthPlane1) + 1;

// The state of each read, or none where it is not known.
using ReadStates = std::array<std::optional<std::uint32_t>, READ_COUNT>;

// The states RR expects at the TAA draw: T, the velocity, the albedo and the normals in SHADER_RESOURCE,
// and the two planes of the depth in DEPTH_READ | SHADER_RESOURCE. The game leaves the normals and the
// depth so in an earlier submission of the same frame, and the TAA's list does not touch them.
std::uint32_t expected_state(Read read);

// The resources of the reads at one TAA draw.
struct ReadResources {
    ResourceId t{};
    GBuffer gbuffer{};
};

// The anchored draw of the TAA as its recording keeps it, for the confirmation at its submission.
struct TaaMarker {
    ReadResources resources{};
    std::uint64_t gbuffer_pass{}; // the number of the G-buffer pass the draw used
    ReadStates predicted{};
    bool known{}; // the draw took the state as known

    // False in a frame without velocity, whose TAA reads none.
    bool reads_velocity{true};
};

// What a command list records that the registry replays at its submission, in order. A split barrier
// counts at its END half only.
class Recording {
public:
    struct Event {
        enum class Kind : std::uint8_t {
            Transition,
            GBufferPass,
            TaaDraw,
        };

        Kind kind{};
        Transition transition{};      // a Transition
        std::uint64_t gbuffer_pass{}; // the number of a GBufferPass
    };

    // Reset of the list, or a new list: a new recording. Keeps the storage: the game resets its lists
    // every frame.
    void reset();

    void transition(const Transition& transition);
    void gbuffer_pass(std::uint64_t number);

    // The barrier rule attempts once per recording, so a recording has at most one.
    void taa_draw(const TaaMarker& marker);

    // Where the last transition of this subresource in the recording left it, ALL_SUBRESOURCES
    // included. None if the recording has not transitioned it.
    std::optional<std::uint32_t> state(ResourceId resource, std::uint32_t subresource) const;

    // Whether a transition of the recording names the resource, which is then alive.
    bool names(ResourceId resource) const;

    const std::vector<Event>& events() const { return m_events; }
    const std::optional<TaaMarker>& taa() const { return m_taa; }

private:
    std::vector<Event> m_events{};
    std::optional<TaaMarker> m_taa{};
};

// Why the state of the reads is not known at a TAA draw. When several apply, the first one wins.
enum class StateProblem : std::uint8_t {
    None,
    // No G-buffer pass identified yet.
    GBufferUnknown,
    // The velocity the barrier rule found is not the G-buffer's, as with a G-buffer that was recreated
    // and that no pass has named yet.
    VelocityNotGBuffer,
    // The G-buffer is not at T's size.
    SizeNotT,
    // Another G-buffer than at the last TAA draw: RR is omitted in the frame where the identity changes.
    // The first draw counts as a change.
    IdentityChanged,
    // A mismatch on a read, or a state predicted at a draw that was not the one at its TAA, since the
    // last clean confirmation.
    Mismatch,
    // A read has no known state.
    Unknown,
    // A read is known in another state than the expected one.
    Unexpected,
};

// From the last problem, which a new one has to replace here.
inline constexpr std::size_t STATE_PROBLEM_COUNT = static_cast<std::size_t>(StateProblem::Unexpected) + 1;

struct StateAssessment {
    StateProblem problem{};
    std::optional<GBuffer> gbuffer{}; // the G-buffer the draw used
    std::uint64_t gbuffer_pass{};     // the number of its pass
    bool gbuffer_submitted{};         // the list of that pass was submitted before the draw
    ReadStates predicted{};           // where the TAA's list will find the reads, if nothing else moves them
};

// The order between the list of the G-buffer pass and the TAA's, as the TAA's submission shows it.
enum class GBufferOrder : std::uint8_t {
    // The pass the draw used is the last one submitted before the TAA's list.
    SamePass,
    // A pass recorded after the draw was submitted before the TAA's list: the draw used a stale one.
    LaterPass,
    // The pass the draw used was not submitted before the TAA's list.
    NotSubmitted,
};

inline constexpr std::size_t GBUFFER_ORDER_COUNT = static_cast<std::size_t>(GBufferOrder::NotSubmitted) + 1;

// What the submission of a TAA's list confirmed.
struct TaaConfirmation {
    TaaMarker marker{};
    ReadStates actual{}; // at the TAA, in submission order
    GBufferOrder order{};

    // The last G-buffer pass submitted before the TAA's list names another G-buffer than the draw used:
    // the draw took resources that the frame no longer uses, whatever their states say.
    bool gbuffer_differs{};

    // The G-buffer differs, or a state predicted at the draw is not the one at the TAA.
    bool disagree{};

    // Every read was in its expected state at the TAA.
    bool expected{};
};

// A mismatch on a resource RR reads, for the log.
struct ReadMismatch {
    Read read{};
    ResourceId resource{};
    std::uint32_t subresource{};
    std::uint32_t before{}; // what the game's barrier says
    std::uint32_t held{};   // what the registry had
};

struct Submission {
    std::optional<TaaConfirmation> taa{};
    std::vector<ReadMismatch> read_mismatches{};
};

struct StateCounters {
    std::uint64_t created{};
    std::uint64_t lists{};
    std::uint64_t transitions{}; // replayed on the resources followed
    std::uint64_t first_sights{};
    std::uint64_t mismatches{};
    std::uint64_t read_mismatches{}; // of them, on what RR reads
    std::uint64_t not_followed{};    // on buffers and simultaneous-access textures
    std::uint64_t unseen_creation{}; // on resources whose creation the registry did not see
    std::uint64_t copy_list{};       // on copy lists, which leave the state unknown

    std::uint64_t gbuffer_passes{};
    std::uint64_t gbuffer_changes{};

    std::uint64_t draws{};
    std::uint64_t draws_pass_submitted{};
    std::array<std::uint64_t, STATE_PROBLEM_COUNT> problems{}; // by the problem of each draw

    std::uint64_t confirmations{};
    std::array<std::uint64_t, READ_COUNT> expected_at_taa{}; // by read, with its expected state at the TAA
    std::array<std::uint64_t, GBUFFER_ORDER_COUNT> order{};
    std::uint64_t disagree{};
    std::uint64_t gbuffer_differs{}; // of them, with another G-buffer submitted than the draw used
    std::uint64_t wrong{};           // of them, at a draw that took the state as known: never acceptable
};

// The registry of the states, the G-buffer's identity and the judgement at each TAA draw. Not
// thread-safe: the hooks serialize every call with one mutex.
class ResourceStates {
public:
    // A resource created at this address, with this many subresources (mips, array slices and planes).
    // What was known of an earlier one there is gone. Only textures without simultaneous access keep
    // their state between submissions, so the others are not followed.
    void on_created(ResourceId resource, bool followed, std::uint32_t subresources = 1);

    struct Pass {
        std::uint64_t number{}; // the list that records the pass records it too
        bool changed{};         // another identity than the last pass's
    };

    // A G-buffer pass, as a list records it.
    Pass on_gbuffer_pass(const GBuffer& gbuffer);

    // At the draw the barrier rule anchored: whether the state of the reads is known. velocity is the
    // one the barrier rule found, or none at the TAA draw of a frame without velocity: RR does not read
    // the G-buffer's, which is then neither checked nor predicted. The draw goes into the list's
    // recording, for the confirmation.
    StateAssessment on_taa_draw(ResourceId t, ResourceId velocity, Extent t_size, Recording& list);

    // A list submitted with ExecuteCommandLists, in submission order. Transitions on a copy list leave
    // the state unknown: what a copy queue touches decays to COMMON.
    Submission on_submitted(const Recording& list, bool copy_list);

    std::optional<std::uint32_t> state(ResourceId resource, std::uint32_t subresource) const;

    // The last G-buffer pass identified.
    const std::optional<GBuffer>& gbuffer() const { return m_gbuffer; }

    const StateCounters& counters() const { return m_counters; }
    void reset_counters() { m_counters = {}; }

private:
    // One resource: the state of all its subresources, and the subresources that left it.
    struct Tracked {
        bool followed{};
        std::uint32_t count{1}; // of subresources
        std::optional<std::uint32_t> all{};
        std::vector<std::pair<std::uint32_t, std::optional<std::uint32_t>>> apart{};

        std::optional<std::uint32_t> get(std::uint32_t subresource) const;
        void set(std::uint32_t subresource, std::optional<std::uint32_t> state);

        // The known state that the transition's StateBefore contradicts, if any; for ALL_SUBRESOURCES,
        // that of any subresource. known says whether there was a known state to check against.
        std::optional<std::uint32_t> contradicted(std::uint32_t subresource, std::uint32_t before, bool& known) const;
    };

    std::optional<std::uint32_t> state_of(Read read, const ReadResources& resources) const;

    // What a subresource is to RR, by the last TAA draw's T and the last G-buffer pass.
    std::optional<Read> read_of(ResourceId resource, std::uint32_t subresource) const;
    void replay(const Transition& transition, bool copy_list, Submission& submission);
    TaaConfirmation confirm(const TaaMarker& marker);

    std::unordered_map<ResourceId, Tracked> m_resources{};

    std::optional<GBuffer> m_gbuffer{};
    std::uint64_t m_pass{};           // the number of the last G-buffer pass recorded
    std::uint64_t m_submitted_pass{}; // the highest number submitted

    // The last G-buffer passes recorded, by number, so that a submission knows what its pass named; and
    // the G-buffer of the last pass submitted.
    std::deque<std::pair<std::uint64_t, GBuffer>> m_recorded_passes{};
    std::optional<GBuffer> m_submitted_gbuffer{};

    std::optional<GBuffer> m_draw_gbuffer{}; // the G-buffer at the last TAA draw
    ResourceId m_t{};                        // T at the last TAA draw

    // A mismatch on a read, or a wrong prediction, has not had a clean confirmation since.
    bool m_unconfirmed{};

    // A mismatch on a read since the last confirmation.
    bool m_mismatch_since_taa{};

    StateCounters m_counters{};
};

// How many frames a G-buffer stays alive after the last draw that may have read it: far more than the
// game keeps in flight, as the guide pass waits for its own resources.
inline constexpr std::uint64_t GBUFFER_HOLD_FRAMES = 16;

// Keeps the resources of the last G-buffer alive for the guide pass. The registry knows them only by
// their addresses, a pass names them only while its list records it, and the game can free them after
// that: the TAA's list does not name the normals or the depth, and a frame can reuse an earlier pass.
// Without this, the guide pass could describe and view freed resources.
//
// Set: the owning references of one G-buffer. Once replaced, they may be the last ones, so a set that a
// draw may have read waits until no list in flight can read it, and a set no draw read goes at once. What
// take_expired returns has to be destroyed outside the registry's lock, since that can destroy the
// resources. Not thread-safe: it goes under the same lock as the registry, so that the G-buffer a draw
// gets and the set it marks as read are the same one.
template <typename Set>
class GBufferHold {
public:
    // A G-buffer pass with another identity than the last one.
    void replace(Set set) {
        if (m_current) {
            m_replaced.push_back({std::move(*m_current), m_read_at ? *m_read_at + GBUFFER_HOLD_FRAMES : 0});
        }

        m_current = std::move(set);
        m_read_at.reset();
    }

    // A TAA draw of this frame may read the set held.
    void on_read(std::uint64_t frame) { m_read_at = frame; }

    // The sets replaced that no list in flight can read at this frame.
    std::vector<Set> take_expired(std::uint64_t frame) {
        std::vector<Set> expired{};
        const auto first = std::stable_partition(
            m_replaced.begin(), m_replaced.end(), [frame](const Replaced& replaced) { return frame < replaced.until; });

        for (auto it = first; it != m_replaced.end(); ++it) {
            expired.push_back(std::move(it->set));
        }

        m_replaced.erase(first, m_replaced.end());
        return expired;
    }

private:
    struct Replaced {
        Set set{};
        std::uint64_t until{}; // the first frame at which it can go
    };

    std::optional<Set> m_current{};
    std::optional<std::uint64_t> m_read_at{};
    std::vector<Replaced> m_replaced{};
};

std::string_view to_string(Read read);
std::string_view to_string(StateProblem problem);
std::string_view to_string(GBufferOrder order);
} // namespace rr::core
