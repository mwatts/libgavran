#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>

#include "transactions.h"

struct selected_range
{
    size_t position;
    size_t size_available;
};

struct range_finder
{
    // input
    uint64_t *bitmap;
    size_t bitmap_size;
    size_t size_required;
    size_t index;

    // output
    struct selected_range selection;

    // the current word we are working on
    uint64_t current;

    // state
    uint64_t current_set_bit;
    uint64_t previous_set_bit;
};

static bool handle_zero_word(struct range_finder *restrict range)
{
    range->current_set_bit = (range->index + 1) * 64;
    if (range->current_set_bit > range->previous_set_bit + range->size_required)
    {
        range->selection.position =
            range->previous_set_bit + 1; // intentionally overflowing here
        range->selection.size_available =
            (range->current_set_bit - range->selection.position);
        return true;
    }
    return false;
}

static bool find_range_once(struct range_finder *restrict range)
{
    uint64_t bitset = range->current;

    if (bitset == ULONG_MAX)
    {
        // all bits are set, can skip whole thing
        range->previous_set_bit = (range->index + 1) * 64 - 1;
        return false;
    }

    if (bitset == 0)
    {
        return handle_zero_word(range);
    }

    while (bitset != 0)
    {
        int r = __builtin_ctzl(bitset);
        range->current_set_bit = (range->index) * 64 + (uint64_t)r;
        if (range->current_set_bit >
            range->previous_set_bit + range->size_required)
        {
            // intentionally overflowing here
            range->selection.position = range->previous_set_bit + 1;
            range->selection.size_available =
                (range->current_set_bit - range->selection.position);
            range->previous_set_bit = range->current_set_bit;
            return true;
        }
        range->previous_set_bit = range->current_set_bit;
        bitset ^= (bitset & -bitset);
    }

    return handle_zero_word(range);
}

static void init_range(uint64_t *bitmap, size_t bitmap_size,
                       size_t size_required,
                       struct range_finder *restrict range)
{
    range->bitmap = bitmap;
    range->bitmap_size = bitmap_size;
    range->size_required = size_required;
    range->current = bitmap[0];
    range->previous_set_bit = ULONG_MAX;
    range->index = 0;
}

static bool find_next_range(struct range_finder *restrict range)
{
    do
    {
        if (find_range_once(range))
        {
            if (range->current_set_bit % 64)
            {
                // mask the already found item
                uint64_t mask = ~(ULONG_MAX << (range->current_set_bit % 64));
                range->current |= mask;
            }
            else
            {
                // run out in the current word, but maybe we
                // have more in the next?
                if (range->index + 1 < range->bitmap_size)
                {
                    range->index++;
                    range->current = range->bitmap[range->index];
                    continue;
                }
                else
                {
                    range->current = ULONG_MAX;
                }
            }
            return true;
        }
        range->index++;
        if (range->index >= range->bitmap_size)
            return false;
        range->current = range->bitmap[range->index];
    } while (true);
}

#define MAX_DISTANCE_TO_SEARCH_BEST_MATCH 64

static bool find_smallest_nearby_range(struct range_finder *restrict range, bool search_nearby)
{
    struct selected_range current = {0, SIZE_MAX};
    size_t boundary =
        MAX_DISTANCE_TO_SEARCH_BEST_MATCH +
        // the bigger the request range, the less we care about locality
        +range->size_required;

    while (find_next_range(range))
    {
        if (range->size_required == range->selection.size_available)
            return true;
        if (current.size_available > range->selection.size_available)
        {
            current = range->selection;
        }
        if (search_nearby && range->selection.position > boundary)
        {
            // We have gone too far? Stop being choosy
            if (current.size_available < range->selection.size_available)
            {
                range->selection = current;
            }
            return true;
        }
    }

    range->selection = current;

    return current.size_available != SIZE_MAX;
}

bool find_free_range_in_bitmap(uint64_t *bitmap, size_t bitmap_size,
                               size_t size_required, size_t near_pos,
                               size_t *bit_pos)
{
    if (!size_required || near_pos / 64 >= bitmap_size)
        return false;

    size_t high = near_pos / 64;

    struct range_finder range;
    init_range(bitmap + high, bitmap_size - high, size_required, &range);

    if (find_smallest_nearby_range(&range, high != 0))
    {
        *bit_pos = range.selection.position + high * 64;
        return true;
    }
    if (!high)
    {
        return false; // already scanned it all
    }

    // we search _high_, couldn't find anything, maybe lower?
    init_range(bitmap, high, size_required, &range);
    if (find_next_range(&range))
    {
        *bit_pos = range.selection.position;
        return true;
    }

    return false;
}
