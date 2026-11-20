#include "model/edits.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "config.h"
#include "model/clip_envelope.h"

namespace edits {

static int64_t rounded(double v) { return (int64_t)std::nearbyint(v); }  // Python's round()

ClipPtr cutPiece(const ClipPtr& clip, int64_t lo, int64_t hi) {
    int64_t fadeIn, fadeOut;
    clip->fades(fadeIn, fadeOut);
    int64_t lead = std::max<int64_t>(0, clip->fadeLead) + lo - clip->start;
    int64_t tail = std::max<int64_t>(0, clip->fadeTail) + clip->end() - hi;
    ClipPtr c = copyClip(clip);
    c->start = lo;
    c->offset = clip->offset + lo - clip->start;
    c->length = hi - lo;
    c->fadeIn = fadeIn > lead ? fadeIn : 0;
    c->fadeOut = fadeOut > tail ? fadeOut : 0;
    c->fadeLead = lead;
    c->fadeTail = tail;
    return c;
}

bool splitClip(const ClipPtr& clip, int64_t frame, ClipPtr& left, ClipPtr& right) {
    int64_t cut = frame - clip->start;
    if (!(0 < cut && cut < clip->frames())) return false;
    left = cutPiece(clip, clip->start, frame);
    right = cutPiece(clip, frame, clip->end());
    return true;
}

std::vector<ClipPtr> splitClips(Project& p, const std::vector<ClipPtr>& clips, std::vector<int64_t> frames) {
    std::sort(frames.begin(), frames.end());
    std::map<Clip*, std::vector<ClipPtr>> piecesOf;
    std::vector<Clip*> order;
    for (auto& clip : clips) {
        if (piecesOf.count(clip.get())) continue;
        std::vector<ClipPtr> pieces{clip};
        for (int64_t f : frames) {
            ClipPtr l, r;
            if (splitClip(pieces.back(), f, l, r)) {
                pieces.back() = l;
                pieces.push_back(r);
            }
        }
        piecesOf[clip.get()] = pieces;
        order.push_back(clip.get());
    }
    std::vector<ClipPtr> all;
    for (auto& c : p.clips) {
        auto it = piecesOf.find(c.get());
        if (it == piecesOf.end()) all.push_back(c);
        else all.insert(all.end(), it->second.begin(), it->second.end());
    }
    p.clips = all;
    std::vector<ClipPtr> out;
    for (Clip* c : order) out.insert(out.end(), piecesOf[c].begin(), piecesOf[c].end());
    return out;
}

void clearUnder(Project& p, const std::vector<ClipPtr>& clips) {
    std::set<Clip*> placed;
    for (auto& c : clips) placed.insert(c.get());
    for (auto& clip : clips) {
        std::vector<ClipPtr> covered;
        for (auto& c : p.clipsIn({clip->track}, clip->start, clip->end()))
            if (!placed.count(c.get())) covered.push_back(c);
        if (covered.empty()) continue;
        auto pieces = splitClips(p, covered, {clip->start, clip->end()});
        std::vector<ClipPtr> gone;
        for (auto& piece : pieces)
            if (piece->start >= clip->start && piece->end() <= clip->end()) gone.push_back(piece);
        p.removeClips(gone);
    }
}

void deleteRange(Project& p, const std::vector<TrackPtr>& tracks, int64_t start, int64_t end) {
    auto pieces = splitClips(p, p.clipsIn(tracks, start, end), {start, end});
    std::vector<ClipPtr> gone;
    for (auto& piece : pieces)
        if (piece->start >= start && piece->end() <= end) gone.push_back(piece);
    p.removeClips(gone);
}

ClipPtr pieceWithin(const ClipPtr& clip, int64_t start, int64_t end) {
    int64_t lo = std::max(start, clip->start), hi = std::min(end, clip->end());
    if (hi <= lo) return nullptr;
    return cutPiece(clip, lo, hi);
}

std::vector<ClipPtr> placeCopies(Project& p, const std::vector<ClipPtr>& copies) {
    std::vector<ClipPtr> all = p.clips.copy();
    all.insert(all.end(), copies.begin(), copies.end());
    p.clips = all;
    clearUnder(p, copies);
    return copies;
}

std::vector<ClipPtr> duplicateRange(Project& p, const std::vector<TrackPtr>& tracks, int64_t start, int64_t end) {
    int64_t span = end - start;
    std::vector<ClipPtr> copies;
    for (auto& c : p.clipsIn(tracks, start, end))
        if (ClipPtr piece = pieceWithin(c, start, end)) {
            piece->start += span;
            piece->link = nullptr;  // (a copy: independent of a linked clip it came from)
            copies.push_back(piece);
        }
    return placeCopies(p, copies);
}

std::vector<ClipPtr> duplicateClips(Project& p, const std::vector<ClipPtr>& clips) {
    if (clips.empty()) return {};
    int64_t first = clips[0]->start, last = clips[0]->end();
    for (auto& c : clips) first = std::min(first, c->start), last = std::max(last, c->end());
    int64_t span = last - first;
    std::vector<ClipPtr> copies;
    for (auto& c : clips) {
        ClipPtr k = links::plainCopy(c);  // (independent of a linked clip it came from)
        k->start += span;
        copies.push_back(k);
    }
    return placeCopies(p, copies);
}

ClipPtr restretched(const ClipPtr& clip, double stretch, double pitch, int sr) {
    double k = stretch / clip->stretch;
    ClipPtr c = copyClip(clip);
    c->stretch = stretch;
    c->pitch = pitch;
    c->offset = rounded(clip->offset * k);
    c->loopStart = clip->loopStart * k;
    c->loopLength = clip->loopLength * k;
    c->length = clip->length < 0 ? -1 : std::max<int64_t>(1, rounded(clip->length * k));
    c->fadeIn = rounded(clip->fadeIn * k);
    c->fadeOut = rounded(clip->fadeOut * k);
    c->fadeLead = rounded(clip->fadeLead * k);
    c->fadeTail = rounded(clip->fadeTail * k);
    if (auto envelope = clip->gainEnvelope) c->gainEnvelope = clip_envelope::make(clip_envelope::scaled(*envelope, k));
    c->data = nullptr;
    c->peaks = nullptr;
    c->prepare(sr);
    if (c->length >= 0 && c->data) c->length = std::min(c->length, c->data->frames() - c->offset);
    return c;
}

ClipPtr reversedClip(const ClipPtr& clip, int sr) {
    ClipPtr c = copyClip(clip);
    c->reverse = !clip->reverse;
    c->fadeIn = clip->fadeOut;
    c->fadeOut = clip->fadeIn;
    c->fadeLead = clip->fadeTail;
    c->fadeTail = clip->fadeLead;
    // (heard backwards, a fade-in is a fade-out: a fast rise becomes a late, quick fall)
    c->fadeInShape = mirroredFade(clip->fadeOutShape);
    c->fadeOutShape = mirroredFade(clip->fadeInShape);
    if (auto envelope = clip->gainEnvelope)  // (on the same sound, heard the other way round)
        c->gainEnvelope = clip_envelope::make(clip_envelope::mirrored(*envelope, clip->baseFrames));
    c->data = nullptr;
    c->peaks = nullptr;
    if (clip->loop) {
        // (its audio past the loop is repeats: the loop the other way round, the window on it as before)
        double total = (double)clip->baseFrames / sr;
        double start = std::max(0.0, total - clip->loopStart - clip->loopLength);
        int64_t a = rounded(clip->loopStart * sr), n = std::max<int64_t>(1, rounded(clip->loopLength * sr));
        int64_t into = clip->offset >= a ? (clip->offset - a) % n : 0;  // (where in the loop the window starts)
        c->loopStart = start;
        c->offset = rounded(start * sr) + (n - into) % n;
    } else {
        int64_t frames = clip->frames();
        c->offset = (clip->data ? clip->data->frames() : 0) - clip->offset - frames;
        c->length = frames;
    }
    c->prepare(sr);
    return c;
}

// mixer.py's add_clip: the part of clip in buf (stereo, from timeline frame `start`), with its gain and
// fades and the short fade at its edges
static void addClip(std::vector<float>& buf, const Clip& clip, int64_t start) {
    if (!clip.data) return;
    int64_t bufFrames = (int64_t)buf.size() / 2;
    int64_t cs = clip.start, frames = std::min(clip.frames(), clip.data->frames() - clip.offset);
    int64_t a = std::max(start, cs), b = std::min(start + bufFrames, cs + frames);
    const int fade = config::EDGE_FADE_FRAMES;
    // (its gain envelope walked once, as the mixer does, rather than looked up again at every frame by level())
    Clip plain = clip;
    plain.gainEnvelope = nullptr;
    auto envelope = clip.gainEnvelope;
    std::vector<AutoPoint> none;
    clip_envelope::Walk walk{clip, envelope ? *envelope : none};
    for (int64_t t = a; t < b; t++) {
        int64_t i = t - cs;
        double level = plain.level(i) * (envelope ? walk.gain(clip.offset + i) : 1.0f);
        if (i < fade || i > frames - fade)
            level *= std::min(1.0, (double)std::min(i + 1, frames - i) / fade);
        const float* src = clip.data->frame(clip.offset + i);
        buf[(size_t)(t - start) * 2] += (float)(src[0] * level);
        buf[(size_t)(t - start) * 2 + 1] += (float)(src[1] * level);
    }
}

static bool noteLess(const Note& a, const Note& b) {
    if (a.start != b.start) return a.start < b.start;
    if (a.end != b.end) return a.end < b.end;
    if (a.pitch != b.pitch) return a.pitch < b.pitch;
    return a.velocity < b.velocity;
}

ClipPtr consolidated(const std::vector<ClipPtr>& clips, int sr) {
    if (clips.empty()) return nullptr;
    int64_t start = clips[0]->start, end = clips[0]->end();
    ClipPtr first = clips[0];
    for (auto& c : clips) {
        start = std::min(start, c->start);
        end = std::max(end, c->end());
        if (c->start < first->start) first = c;
    }
    auto c = std::make_shared<Clip>();
    c->name = first->name + " (joined)";
    c->start = start;
    c->color = first->color;
    c->tone = first->tone;
    c->track = first->track;
    if (first->kind != Kind::Midi) {
        c->kind = Kind::Audio;
        auto audio = std::make_shared<AudioBuffer>();
        audio->samples.assign((size_t)(end - start) * 2, 0.0f);
        for (auto& clip : clips) addClip(audio->samples, *clip, start);
        c->source = audio;
        c->sourceSr = sr;
    } else {
        c->kind = Kind::Midi;
        c->effectsCommitted = first->effectsCommitted;  // (callers join only clips that agree: see the header)
        std::vector<Note> notes, muted;
        for (auto& clip : clips) {
            double shift = (double)(clip->start - start) / sr;
            for (auto n : clip->heardNotes(sr)) {
                n.start += shift;
                n.end += shift;
                notes.push_back(n);
            }
            double lo = (double)clip->offset / sr, hi = (double)(clip->offset + clip->frames()) / sr;
            if (clip->mutedNotes)
                for (auto n : *clip->mutedNotes)
                    if (n.start < hi && n.end > lo) {
                        n.start = std::max(n.start, lo) - lo + shift;
                        n.end = std::min(n.end, hi) - lo + shift;
                        muted.push_back(n);
                    }
        }
        std::sort(notes.begin(), notes.end(), noteLess);
        std::sort(muted.begin(), muted.end(), noteLess);
        c->notes = std::make_shared<const std::vector<Note>>(std::move(notes));
        c->mutedNotes = std::make_shared<const std::vector<Note>>(std::move(muted));
        c->length = end - start;
        c->lengthS = (double)(end - start) / sr;
    }
    c->prepare(sr);
    return c;
}

bool crossfaded(const ClipPtr& left, const ClipPtr& right, int64_t frames, ClipPtr& newLeft, ClipPtr& newRight) {
    if (!left->data || !right->data) return false;
    int64_t half = frames / 2;
    int64_t mid = (left->end() + right->start) / 2;
    if ((left->end() + right->start) < 0 && (left->end() + right->start) % 2) mid--;  // (Python's floor division)
    // (audio after left's end; audio before right's start, not before 0 s)
    int64_t reachLeft = std::min(mid + half - left->end(), left->data->frames() - left->offset - left->frames());
    int64_t reachRight = std::min(right->start - std::max<int64_t>(0, mid - half), right->offset);
    reachLeft = std::max(reachLeft, 1 - left->frames());  // (never gone)
    reachRight = std::max(reachRight, 1 - right->frames());
    if (left->end() + reachLeft <= right->start - reachRight) return false;
    newLeft = copyClip(left);
    newLeft->length = left->frames() + reachLeft;
    newRight = copyClip(right);
    newRight->start = right->start - reachRight;
    newRight->offset = right->offset - reachRight;
    newRight->length = right->frames() + reachRight;
    int64_t overlap = newLeft->end() - newRight->start;
    newLeft->fadeOut = overlap;
    newRight->fadeIn = overlap;
    newLeft->fadeTail = 0;
    newRight->fadeLead = 0;
    // (equal power: as loud all through the overlap; model.h FADE SHAPES)
    newLeft->fadeOutShape = FadeShape::EqualPower;
    newRight->fadeInShape = FadeShape::EqualPower;
    return true;
}

}  // namespace edits
