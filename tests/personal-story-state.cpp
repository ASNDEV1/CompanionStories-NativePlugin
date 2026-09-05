#include "../SKSE/src/PersonalStoryState.h"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace IntelEngine::PersonalStories;

void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
Story Prototype(Kind kind, const std::string& id = "project") {
    Story s;
    s.id = id; s.kind = kind; s.owner = 0x100; s.participants = {0x14};
    s.destination = 0x200; s.target = kind == Kind::Reunion ? 0x300 : 0;
    s.offeredAt = 5.0f; s.deadline = 8.0f;
    s.reason = "A concrete personal reason";
    return s;
}
void See(Story& s, Observation kind, uint64_t id, float time) {
    Require(Observe(s, {kind,id,time}, s.revision) == Result::Applied, "expected evidence transition");
    Require(Valid(s), "observation preserves invariants");
}
void ExpeditionTests() {
    auto s = Prototype(Kind::Expedition);
    Require(Observe(s,{Observation::EnteredTogether,1,6},s.revision)==Result::InvalidTransition, "proposal cannot accumulate accomplishments");
    Require(Accept(s,s.revision,6)==Result::Applied, "accept expedition");
    Require(Reflect(s,{Observation::ReflectionDialogue,2,6},s.revision,"We did it") == Result::InvalidTransition, "narration cannot complete active goal");
    Require(Observe(s,{Observation::ClearedTogether,3,6},s.revision)==Result::InvalidEvidence, "cannot clear before entering together");
    auto offerRevision=s.revision;
    See(s,Observation::EnteredTogether,4,6.1f);
    Require(Observe(s,{Observation::EnteredTogether,4,6.1f},offerRevision)==Result::NoChange, "duplicate event idempotent with original revision");
    Require(Observe(s,{Observation::ExitedTogether,5,6.2f},offerRevision)==Result::StaleRevision, "stale decisions rejected");
    Require(Pause(s,s.revision)==Result::Applied, "pause");
    auto pausedMilestones=s.milestones;
    Require(Observe(s,{Observation::ExitedTogether,5,6.2f},s.revision)==Result::InvalidTransition, "paused goal ignores progress");
    Require(Defer(s,s.revision,30)==Result::Applied && s.phase==Phase::Paused && s.milestones==pausedMilestones, "defer preserves work and pause");
    Require(Resume(s,s.revision)==Result::Applied, "resume");
    See(s,Observation::ExitedTogether,6,31); // Appointment lapse does not fabricate failure.
    Require(s.phase==Phase::ReadyForReflection && s.outcome==Outcome::ExpeditionVisited, "visiting without clearing is explicit outcome");
    Require(Observe(s,{Observation::EnteredTogether,4,6.1f},offerRevision)==Result::NoChange, "old duplicate remains idempotent after later milestones");
    Require(Reflect(s,{Observation::ReflectionDialogue,6,31},s.revision,"Visit mattered") == Result::InvalidEvidence, "reflection needs a distinct conversation");
    Require(Reflect(s,{Observation::ReflectionDialogue,7,31.1f},s.revision,"Visit mattered") == Result::Applied, "reflection completes ready story");
    auto completedRevision=s.revision;
    Require(Reflect(s,{Observation::ReflectionDialogue,7,31.1f},completedRevision-1,"duplicate") == Result::NoChange && s.aftermath=="Visit mattered", "duplicate reflection never rewrites aftermath");
    Store store{{s}};
    Require(Offer(store,Prototype(Kind::Expedition))==Result::NoChange && store.stories.size()==1, "completed project never reoffered");
    auto cleared=Prototype(Kind::Expedition,"clear");
    Require(Accept(cleared,cleared.revision,6)==Result::Applied,"accept second expedition");
    See(cleared,Observation::EnteredTogether,10,6);
    See(cleared,Observation::ClearedTogether,11,7);
    Require(cleared.outcome==Outcome::ExpeditionCleared,"cleared outcome differs from visit");
}
void SocialTests() {
    auto gathering=Prototype(Kind::Gathering);
    Require(Accept(gathering,gathering.revision,6)==Result::Applied,"accept gathering");
    Require(Observe(gathering,{Observation::RequiredParticipantsPresent,1,6},gathering.revision)==Result::InvalidEvidence,"attendance needs observed supplies");
    See(gathering,Observation::SuppliesObserved,2,6);
    Require(Observe(gathering,{Observation::TwoWayDialogue,3,6},gathering.revision)==Result::InvalidEvidence,"dialogue needs actual attendance");
    See(gathering,Observation::RequiredParticipantsPresent,4,7);
    See(gathering,Observation::TwoWayDialogue,5,7.1f);
    Require(gathering.outcome==Outcome::GatheringHeld && gathering.phase==Phase::ReadyForReflection,"gathering held, never invented eating");
    auto reunion=Prototype(Kind::Reunion);
    reunion.participants.clear(); reunion.destination=0; // Counterpart is sufficient; no invented appointment.
    Require(Accept(reunion,reunion.revision,6)==Result::Applied,"accept reunion");
    Require(Observe(reunion,{Observation::TwoWayDialogue,1,6},reunion.revision)==Result::InvalidEvidence,"reunion requires jointly present actors");
    See(reunion,Observation::RequiredParticipantsPresent,2,6);
    Require(reunion.phase==Phase::Active,"proximity alone is not a reunion conversation");
    See(reunion,Observation::TwoWayDialogue,3,7);
    Require(reunion.outcome==Outcome::ReunionConversation && reunion.phase==Phase::ReadyForReflection,"two-way reunion dialogue unlocks reflection");
    auto declined=Prototype(Kind::Reunion,"declined");
    Require(Decline(declined,declined.revision)==Result::Applied && Valid(declined),"decline proposal");
    auto failed=Prototype(Kind::Reunion,"failed");
    Require(Fail(failed,failed.revision,Failure::TargetDied)==Result::Applied && Valid(failed),"actual target death is explicit failure");
}
void PersistenceTests() {
    Store early;
    for(auto kind : {Kind::Expedition,Kind::Gathering,Kind::Reunion}) {
        auto s=Prototype(kind,std::to_string(static_cast<uint32_t>(kind)));
        Require(Offer(early,s)==Result::Applied,"offer three independent projects");
    }
    std::vector<unsigned char> a,b;
    Require(Encode(early,a),"encode proposed timeline");
    Store later=early;
    auto& s=later.stories[0];
    Require(Accept(s,s.revision,6)==Result::Applied,"accept stored story");
    See(s,Observation::EnteredTogether,1,6);
    See(s,Observation::ClearedTogether,2,7);
    Require(Reflect(s,{Observation::ReflectionDialogue,3,8},s.revision,"A lasting consequence")==Result::Applied,"complete stored story");
    Require(Encode(later,b),"encode completed timeline");
    Store restored;
    Require(Decode(a,1,restored) && restored.stories[0].phase==Phase::Proposed,"older save restores proposal");
    Require(Decode(b,1,restored) && restored.stories[0].phase==Phase::Completed && restored.stories[0].aftermath=="A lasting consequence","newer save retains completion and aftermath");
    Require(restored.stories[1].phase==Phase::Proposed && restored.stories[2].phase==Phase::Proposed,"other projects unaffected");
    for(size_t length=0;length<b.size();++length) {
        Store sentinel=early;
        Require(!Decode(std::span(b).first(length),1,sentinel),"every truncated record rejected");
        Require(sentinel.stories[0].phase==Phase::Proposed,"decode never publishes partial state");
    }
    Require(!Decode(b,2,restored),"unknown record version rejected");
    auto tail=b; tail.push_back(0);
    Require(!Decode(tail,1,restored),"unexpected trailing fields rejected");
    Store remapped=later;
    Require(RemapForms(remapped,[](uint32_t old,uint32_t& current){current=old+0x1000;return true;}),"remap all engine identities");
    Require(remapped.stories[0].owner==0x1100 && remapped.stories[0].participants[0]==0x1014 && remapped.stories[2].target==0x1300,"owner participant destination and target remapped");
    auto originalOwner=remapped.stories[0].owner;
    Require(!RemapForms(remapped,[](uint32_t old,uint32_t& current){current=old+1;return old!=0x1200;}),"missing form fails remap");
    Require(remapped.stories[0].owner==originalOwner,"failed remap is atomic");
    auto bad=later; bad.stories[0].phase=Phase::Active;
    Require(!Encode(bad,a),"inconsistent terminal milestone rejected");
    bad=early; bad.stories[0].reason=std::string(MAX_REASON_BYTES+1,'x');
    Require(!Encode(bad,a),"text storage bounded");
    bad=early; bad.stories[0].deadline=std::numeric_limits<float>::quiet_NaN();
    Require(!Encode(bad,a),"invalid clock rejected");
}
void TransitionInvariantTests() {
    std::mt19937 random(147);
    for(int trace=0;trace<300;++trace) {
        auto s=Prototype(static_cast<Kind>(trace%3));
        for(int step=0;step<50;++step) {
            auto revision=s.revision;
            float time=6.0f+step;
            switch(random()%9) {
            case 0: Accept(s,revision,time);break;
            case 1: Pause(s,revision);break;
            case 2: Resume(s,revision);break;
            case 3: Defer(s,revision,time+5);break;
            case 4: Decline(s,revision);break;
            case 5: Fail(s,revision,Failure::OwnerDied);break;
            case 6: Reflect(s,{Observation::ReflectionDialogue,uint64_t(step+1),time},revision,"afterward");break;
            default: Observe(s,{static_cast<Observation>(random()%6),uint64_t(step+1),time},revision);break;
            }
            Require(Valid(s),"transition trace preserves state invariants");
        }
    }
}
int main() {
    try {
        ExpeditionTests(); SocialTests(); PersistenceTests(); TransitionInvariantTests();
        std::cout<<"Personal stories: three evidence-driven arcs, reflection, revision/idempotency, save isolation, atomic codec/remap and transition invariants passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
