#include "of_focus_stats.h"
#include <stdlib.h>
#include <string.h>

bool ofFocusStatsAdd(OfFocusStats *s, uint64_t total, uint64_t flip,
                     uint64_t present, const OfPerfTotals *t) {
    if (s->count >= OF_FOCUS_SAMPLES || flip > total || present > total - flip ||
        UINT64_MAX - s->total < total || UINT64_MAX - s->flip < flip ||
        UINT64_MAX - s->present < present) return false;
    s->work[s->count++] = total - flip;
    s->total += total; s->flip += flip; s->present += present;
    for (int i=0; i<OF_PHASES; ++i) s->totals.phaseNanos[i] += t->phaseNanos[i];
    for (int i=0; i<SWR_PROF_KINDS; ++i) {
        s->totals.drawNanos[i] += t->drawNanos[i];
        s->totals.drawCalls[i] += t->drawCalls[i];
    }
    for (int i=0; i<OF_LOAD_KINDS; ++i) s->totals.loadNanos[i] += t->loadNanos[i];
    s->totals.frames += t->frames; s->totals.detailFrames += t->detailFrames;
    return true;
}
static int compareNanos(const void *a, const void *b) {
    uint64_t x=*(const uint64_t *)a, y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}
uint64_t ofFocusStatsPercentile(const OfFocusStats *s, unsigned percentile) {
    if (s->count==0) return 0;
    if (percentile>100) percentile=100;
    uint64_t copy[OF_FOCUS_SAMPLES];
    memcpy(copy,s->work,s->count*sizeof(copy[0]));
    qsort(copy,s->count,sizeof(copy[0]),compareNanos);
    unsigned rank=(s->count*percentile+99)/100;
    return copy[rank>0 ? rank-1 : 0];
}
uint64_t ofFocusStatsBlockMean(const OfFocusStats *s, unsigned first, unsigned count) {
    if (first>=s->count || count==0) return 0;
    if (count>s->count-first) count=s->count-first;
    uint64_t sum=0;
    for (unsigned i=0; i<count; ++i) sum+=s->work[first+i];
    return sum/count;
}
