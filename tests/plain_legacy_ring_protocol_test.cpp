// Deterministic protocol model for the non-keyed legacy fallback. The GPU E2E
// test covers the real DLL and driver; this model forces timing/error orders
// that are difficult to reproduce reliably on a fast local adapter.
#include <stdint.h>
#include <stdio.h>

#include "../common/fcs_ipc.hpp"
#include "../hook/fcs_recreate_policy.hpp"

namespace {

enum class QueryResult { NotReady, Complete, Failed };
enum class SlotState { Free, CopyPending, Published };

struct WireState {
    LONG64 generation = 2;
    LONG64 ringId = 100;
    FcsSharingMode mode = FCS_SHARING_LEGACY_PLAIN_HANDLE;
    LONG64 frameId = 0;
    LONG64 slotSequence[FCS_SLOT_COUNT]{};
    LONG64 ackSequence[FCS_SLOT_COUNT]{};
    LONG64 ackRingId[FCS_SLOT_COUNT]{};
};

struct ProducerSlot {
    SlotState state = SlotState::Free;
    LONG64 sequence = 0;
    LONG64 generation = 0;
    LONG64 ringId = 0;
};

struct ProducerModel {
    WireState* wire = nullptr;
    ProducerSlot slots[FCS_SLOT_COUNT]{};
    LONG nextSlot = 0;
    LONG64 nextSequence = 0;
    LONG64 copiesSubmitted = 0;
    LONG64 busyDropped = 0;
    LONG64 queryPolls = 0;
    LONG64 waits = 0;
    LONG64 flushes = 0;
    bool captureEnabled = true;
    bool failed = false;

    explicit ProducerModel(WireState& state) : wire(&state) {}

    bool ExactAck(UINT slot) const {
        if (slots[slot].state != SlotState::Published) return false;
        const LONG64 firstSequence = wire->ackSequence[slot];
        const LONG64 ringId = wire->ackRingId[slot];
        const LONG64 secondSequence = wire->ackSequence[slot];
        return firstSequence == slots[slot].sequence &&
               secondSequence == slots[slot].sequence &&
               ringId == slots[slot].ringId;
    }

    void ReclaimExactAcks() {
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            if (!ExactAck(slot)) continue;
            // Clear the old public sequence before the slot can hold another
            // copy. A same-generation viewer reopen must never see old data.
            wire->slotSequence[slot] = 0;
            slots[slot] = {};
        }
    }

    bool Submit() {
        if (!captureEnabled || failed) return false;
        ReclaimExactAcks();
        LONG chosen = -1;
        for (UINT attempt = 0; attempt < FCS_SLOT_COUNT; ++attempt) {
            const LONG slot = (nextSlot + static_cast<LONG>(attempt)) %
                              static_cast<LONG>(FCS_SLOT_COUNT);
            if (slots[slot].state == SlotState::Free) {
                chosen = slot;
                break;
            }
        }
        if (chosen < 0) {
            ++busyDropped;
            return false;
        }
        ProducerSlot& slot = slots[chosen];
        slot.state = SlotState::CopyPending;
        slot.sequence = ++nextSequence;
        slot.generation = wire->generation;
        slot.ringId = wire->ringId;
        wire->slotSequence[chosen] = 0;
        nextSlot = (chosen + 1) % static_cast<LONG>(FCS_SLOT_COUNT);
        ++copiesSubmitted;
        return true;
    }

    void Poll(UINT slot, QueryResult result) {
        ++queryPolls;
        ProducerSlot& pending = slots[slot];
        if (pending.state != SlotState::CopyPending) return;
        if (!captureEnabled || pending.ringId != wire->ringId ||
            wire->mode != FCS_SHARING_LEGACY_PLAIN_HANDLE) {
            pending = {};
            return;
        }
        if (result == QueryResult::NotReady) return;
        if (result == QueryResult::Failed) {
            pending = {};
            failed = true;
            captureEnabled = false;
            return;
        }
        wire->slotSequence[slot] = pending.sequence;
        pending.state = SlotState::Published;
        if (pending.sequence > wire->frameId) {
            wire->frameId = pending.sequence;
        }
    }

    void Resize(LONG64 replacementGeneration, LONG64 replacementRingId) {
        wire->generation = replacementGeneration;
        wire->ringId = replacementRingId;
        wire->frameId = 0;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            wire->slotSequence[slot] = 0;
            wire->ackSequence[slot] = 0;
            wire->ackRingId[slot] = 0;
            slots[slot] = {};
        }
        nextSlot = 0;
    }

    void LoseHookOrder() { captureEnabled = false; }
};

struct ConsumerModel {
    WireState* wire = nullptr;
    LONG64 consumed[FCS_SLOT_COUNT]{};
    bool pending = false;
    UINT pendingSlot = 0;
    LONG64 pendingSequence = 0;
    LONG64 pendingRingId = 0;
    LONG64 copiesSubmitted = 0;
    LONG64 queryPolls = 0;
    LONG64 waits = 0;
    LONG64 flushes = 0;

    explicit ConsumerModel(WireState& state) : wire(&state) {}

    bool CommitAck(UINT slot, LONG64 ringId, LONG64 sequence) {
        if (wire->mode != FCS_SHARING_LEGACY_PLAIN_HANDLE ||
            wire->ringId != ringId ||
            wire->slotSequence[slot] != sequence) return false;
        wire->ackRingId[slot] = ringId;
        wire->ackSequence[slot] = sequence; // commit field
        return true;
    }

    bool BeginNewest() {
        if (pending) return false;
        LONG newestSlot = -1;
        LONG64 newestSequence = 0;
        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            const LONG64 sequence = wire->slotSequence[slot];
            if (sequence > consumed[slot] && sequence > newestSequence) {
                newestSlot = static_cast<LONG>(slot);
                newestSequence = sequence;
            }
        }
        if (newestSlot < 0) return false;

        for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
            const LONG64 sequence = wire->slotSequence[slot];
            if (static_cast<LONG>(slot) == newestSlot || sequence <= consumed[slot]) {
                continue;
            }
            if (CommitAck(slot, wire->ringId, sequence)) {
                consumed[slot] = sequence;
            }
        }
        pending = true;
        pendingSlot = static_cast<UINT>(newestSlot);
        pendingSequence = newestSequence;
        pendingRingId = wire->ringId;
        ++copiesSubmitted;
        return true;
    }

    bool Poll(QueryResult result, bool& acked) {
        acked = false;
        ++queryPolls;
        if (!pending || result == QueryResult::NotReady) return false;
        if (result == QueryResult::Complete) {
            acked = CommitAck(pendingSlot, pendingRingId, pendingSequence);
            if (acked) consumed[pendingSlot] = pendingSequence;
        }
        pending = false;
        return true;
    }
};

bool Expect(bool value, const char* message) {
    if (value) return true;
    printf("FAIL: %s\n", message);
    return false;
}

bool TestExternalRecreateCooldownPolicy() {
    using fcs::hook::BypassResourceRetryForColdRecreate;
    using fcs::hook::DecideExternalRecreate;

    constexpr int64_t now = 100;
    constexpr int64_t cooldown = 200;
    const auto coldStart =
        DecideExternalRecreate(true, false, now, cooldown);
    if (!Expect(coldStart.consume && !coldStart.releaseLiveGraph,
                "cold recreate was blocked by destructive cooldown")) {
        return false;
    }
    if (!Expect(BypassResourceRetryForColdRecreate(true, false),
                "cold recreate did not bypass an old retry deadline")) {
        return false;
    }

    const auto liveDuringCooldown =
        DecideExternalRecreate(true, true, now, cooldown);
    if (!Expect(!liveDuringCooldown.consume &&
                    !liveDuringCooldown.releaseLiveGraph,
                "live graph was torn down inside recreate cooldown")) {
        return false;
    }
    if (!Expect(!BypassResourceRetryForColdRecreate(true, true),
                "live graph bypassed the normal retry policy")) {
        return false;
    }

    const auto liveAtDeadline =
        DecideExternalRecreate(true, true, cooldown, cooldown);
    if (!Expect(liveAtDeadline.consume &&
                    liveAtDeadline.releaseLiveGraph,
                "live graph was not recreated at the cooldown deadline")) {
        return false;
    }

    const auto noRequest =
        DecideExternalRecreate(false, false, now, cooldown);
    return Expect(!noRequest.consume && !noRequest.releaseLiveGraph &&
                      !BypassResourceRetryForColdRecreate(false, false),
                  "missing recreate request changed resource policy");
}

bool FillAndPublish(ProducerModel& producer) {
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        if (!producer.Submit()) return false;
    }
    for (UINT slot = 0; slot < FCS_SLOT_COUNT; ++slot) {
        producer.Poll(slot, QueryResult::Complete);
    }
    return true;
}

bool TestSlowConsumerAndExactAck() {
    WireState wire;
    ProducerModel producer(wire);
    if (!Expect(FillAndPublish(producer), "could not fill model ring")) return false;
    if (!Expect(producer.copiesSubmitted == FCS_SLOT_COUNT,
                "unexpected initial copy count")) return false;

    for (int i = 0; i < 1000; ++i) producer.Submit();
    if (!Expect(producer.copiesSubmitted == FCS_SLOT_COUNT &&
                producer.busyDropped == 1000,
                "slow consumer did not drop without overwriting")) return false;

    wire.ackRingId[0] = wire.ringId;
    wire.ackSequence[0] = wire.slotSequence[0] + 1;
    producer.ReclaimExactAcks();
    if (!Expect(producer.slots[0].state == SlotState::Published,
                "wrong sequence freed a slot")) return false;
    wire.ackRingId[0] = wire.ringId + 1;
    wire.ackSequence[0] = wire.slotSequence[0];
    producer.ReclaimExactAcks();
    if (!Expect(producer.slots[0].state == SlotState::Published,
                "wrong ring ID freed a slot")) return false;

    wire.ackRingId[0] = wire.ringId;
    wire.ackSequence[0] = wire.slotSequence[0];
    producer.ReclaimExactAcks();
    if (!Expect(producer.slots[0].state == SlotState::Free &&
                wire.slotSequence[0] == 0,
                "exact ACK did not clear the old published sequence")) return false;
    if (!Expect(producer.Submit() && producer.copiesSubmitted == 4,
                "producer did not resume after exact ACK")) return false;
    return Expect(producer.waits == 0 && producer.flushes == 0,
                  "producer used a wait or Flush");
}

bool TestOutOfOrderAndConsumerQuery() {
    WireState wire;
    ProducerModel producer(wire);
    producer.Submit();
    producer.Submit();
    producer.Submit();
    producer.Poll(0, QueryResult::NotReady);
    if (!Expect(wire.slotSequence[0] == 0 && wire.frameId == 0,
                "S_FALSE query was published")) return false;
    producer.Poll(2, QueryResult::Complete);
    const LONG64 frameAfterNewest = wire.frameId;
    producer.Poll(1, QueryResult::Complete);
    producer.Poll(0, QueryResult::Complete);
    if (!Expect(frameAfterNewest == 3 && wire.frameId == 3,
                "out-of-order completions regressed frameId")) return false;

    ConsumerModel consumer(wire);
    if (!Expect(consumer.BeginNewest() && consumer.pendingSequence == 3,
                "consumer did not select newest published sequence")) return false;
    if (!Expect(wire.ackSequence[0] == 1 && wire.ackSequence[1] == 2 &&
                wire.ackSequence[2] == 0,
                "never-read stale slots were not ACKed immediately")) return false;
    bool acked = false;
    for (int poll = 0; poll < 1000; ++poll) {
        consumer.Poll(QueryResult::NotReady, acked);
    }
    if (!Expect(!acked && wire.ackSequence[2] == 0,
                "consumer ACKed before its GPU query completed")) return false;
    // An unrelated metadata publication is not a resource replacement. The
    // stable ring ID and exact sequence remain the ACK identity.
    wire.generation += 2;
    consumer.Poll(QueryResult::Complete, acked);
    if (!Expect(acked && wire.ackSequence[2] == 3,
                "consumer did not ACK after query completion")) return false;
    if (!Expect(consumer.waits == 0 && consumer.flushes == 0,
                "consumer used a wait or Flush")) return false;

    WireState failedWire;
    failedWire.slotSequence[0] = 1;
    ConsumerModel failedConsumer(failedWire);
    if (!Expect(failedConsumer.BeginNewest(),
                "could not queue consumer failure case")) return false;
    bool failedAck = false;
    failedConsumer.Poll(QueryResult::Failed, failedAck);
    return Expect(!failedAck && failedWire.ackSequence[0] == 0,
                  "failed consumer query acknowledged a slot");
}

bool TestResizeRejectsStaleCompletions() {
    WireState wire;
    ProducerModel producer(wire);
    producer.Submit(); // slot 0 pending in generation 2
    producer.Submit(); // slot 1 pending in generation 2
    producer.Poll(1, QueryResult::Complete);
    ConsumerModel consumer(wire);
    if (!Expect(consumer.BeginNewest(), "could not queue old consumer copy")) return false;

    producer.Resize(4, 200);
    producer.Poll(0, QueryResult::Complete);
    bool staleAck = false;
    consumer.Poll(QueryResult::Complete, staleAck);
    if (!Expect(!staleAck && wire.slotSequence[0] == 0 &&
                wire.slotSequence[1] == 0 && wire.ackSequence[1] == 0,
                "old producer or consumer completion escaped into new ring")) return false;

    return Expect(producer.Submit(), "replacement generation could not submit");
}

bool TestLateHookAndQueryFailureFailClosed() {
    WireState wire;
    ProducerModel producer(wire);
    producer.Submit();
    const LONG64 copiesBeforeLoss = producer.copiesSubmitted;
    const LONG64 framesBeforeLoss = wire.frameId;
    producer.LoseHookOrder();
    producer.Poll(0, QueryResult::Complete);
    for (int i = 0; i < 100; ++i) producer.Submit();
    if (!Expect(producer.copiesSubmitted == copiesBeforeLoss &&
                wire.frameId == framesBeforeLoss && wire.slotSequence[0] == 0,
                "pending frame published after late hook loss")) return false;

    WireState failedWire;
    ProducerModel failedProducer(failedWire);
    failedProducer.Submit();
    failedProducer.Poll(0, QueryResult::Failed);
    if (!Expect(failedProducer.failed && !failedProducer.captureEnabled &&
                failedWire.slotSequence[0] == 0,
                "GetData failure did not fail closed")) return false;
    const LONG64 failedCopies = failedProducer.copiesSubmitted;
    failedProducer.Submit();
    return Expect(failedProducer.copiesSubmitted == failedCopies,
                  "producer submitted after query failure");
}

} // namespace

int main() {
    if (!TestExternalRecreateCooldownPolicy()) return 1;
    if (!TestSlowConsumerAndExactAck()) return 1;
    if (!TestOutOfOrderAndConsumerQuery()) return 1;
    if (!TestResizeRejectsStaleCompletions()) return 1;
    if (!TestLateHookAndQueryFailureFailClosed()) return 1;
    printf("PASS: deterministic ring and recreate-cooldown invariants hold; "
           "no waits or Flushes\n");
    return 0;
}
