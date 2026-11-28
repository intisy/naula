#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "layout_apply_internal.h"
#include "naula/json.h"
#include "naula/layout_apply.h"

/**
 * Applies one move, `<from>` to `<to>`, under `<root>` with the memory banks in `<banks>`, stopping as if the process
 * died at boundary `<stop>`, so the command line has an unfinished journal to resume. Exits 0 once stopped there.
 */
int main(int count, char **values) {
    naula_json_use_allocator();
    if (count != 6) {
        fputs("usage: stopped_move <root> <from> <to> <banks> <stop>\n", stderr);
        return 2;
    }
    naula_layout_step step;
    memset(&step, 0, sizeof step);
    step.size = sizeof step;
    step.kind = NAULA_MOVE_CHECKOUT;
    step.path = values[2];
    step.target = values[3];
    naula_layout_plan plan;
    memset(&plan, 0, sizeof plan);
    plan.size = sizeof plan;
    plan.steps = &step;
    plan.step_count = 1;
    naula_apply_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.root = values[1];
    options.memory_banks = values[4];
    naula_refusal refusal;
    memset(&refusal, 0, sizeof refusal);
    refusal.size = sizeof refusal;
    naula_apply_test_stop_at(strtoul(values[5], NULL, 10));
    naula_status status = naula_layout_apply(&plan, &options, &refusal);
    printf("%d %s\n", (int)status, refusal.code);
    return status == NAULA_REFUSED && strcmp(refusal.code, "test-stopped") == 0 ? 0 : 1;
}
