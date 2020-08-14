/*

    Copyright 2018, Chris Chronopoulos

    This file is part of libsequoia.

    libsequoia is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libsequoia is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with libsequoia.  If not, see <https://www.gnu.org/licenses/>.

*/

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#include "sequoia.h"

// <helper>

int _get_tick_index_trig(sq_sequence_t *seq, int step_index, sq_trigger_t *trig) {

    // TODO: trig = seq->trigs + step_index ?

    int tick_index = (step_index + trig->microtime) * seq->tps;
    if (tick_index < 0) tick_index = 0;
    if (tick_index >= seq->nticks) tick_index = seq->nticks - 1;

    return tick_index;

}

void _sequence_ringbuffer_write(sq_sequence_t *seq, _sequence_ctrl_msg_t *msg) {

    int avail = jack_ringbuffer_write_space(seq->rb);
    if (avail < sizeof(_sequence_ctrl_msg_t)) {
        fprintf(stderr, "sequence ringbuffer: overflow\n");
        return;
    }

    jack_ringbuffer_write(seq->rb, (const char*) msg, sizeof(_sequence_ctrl_msg_t));

}

void _sequence_serve_ctrl_msgs(sq_sequence_t *seq) {

    int avail = jack_ringbuffer_read_space(seq->rb);
    _sequence_ctrl_msg_t msg;
    while(avail >= sizeof(_sequence_ctrl_msg_t)) {

        jack_ringbuffer_read(seq->rb, (char*) &msg, sizeof(_sequence_ctrl_msg_t));

        if (msg.param == SEQUENCE_SET_TRIG) {
            _sequence_set_trig_now(seq, msg.vi, msg.vp);
        } else if (msg.param == SEQUENCE_CLEAR_TRIG) {
            _sequence_clear_trig_now(seq, msg.vi);
        } else if (msg.param == SEQUENCE_TRANSPOSE) {
            _sequence_set_transpose_now(seq, msg.vi);
        } else if (msg.param == SEQUENCE_PH) {
            _sequence_set_playhead_now(seq, msg.vi);
        } else if (msg.param == SEQUENCE_FIRST) {
            _sequence_set_first_now(seq, msg.vi);
        } else if (msg.param == SEQUENCE_LAST) {
            _sequence_set_last_now(seq, msg.vi);
        } else if (msg.param == SEQUENCE_DIV) {
            _sequence_set_clockdivide_now(seq, msg.vi);
        } else if (msg.param == SEQUENCE_MUTE) {
            _sequence_set_mute_now(seq, msg.vb);
        }

        avail -= sizeof(_sequence_ctrl_msg_t);

    }

}

// </helper>

sq_sequence_t *sq_sequence_new(int nsteps, int tps) {

    sq_sequence_t *seq;

    seq = malloc(sizeof(sq_sequence_t));

    seq->nsteps = nsteps;
    seq->tps = tps;
    seq->name[0] = '\0';
    seq->transpose = 0;

    seq->div = 1;

    seq->trigs = malloc(seq->nsteps * sizeof(sq_trigger_t));
    for (int i=0; i<seq->nsteps; i++) {
        sq_trigger_init(seq->trigs + i);
    }

    seq->nticks = seq->nsteps * seq->tps;

    seq->microgrid = malloc(seq->nticks * sizeof(sq_trigger_t*));
    for(int i=0; i<seq->nticks; i++) {
        seq->microgrid[i] = NULL;
    }

    seq->buf_off = malloc(seq->nticks * sizeof(midi_packet));
    for(int i=0; i<seq->nticks; i++) {
        seq->buf_off[i][0] = 0;
    }

    // allocate and lock ringbuffer (universal ringbuffer length?)
    seq->rb = jack_ringbuffer_create(RINGBUFFER_LENGTH * sizeof(_sequence_ctrl_msg_t));
    int err = jack_ringbuffer_mlock(seq->rb);
    if (err) {
        fprintf(stderr, "failed to lock ringbuffer\n");
        exit(1);
    }

    seq->is_playing = false;

    seq->outport = NULL;

    seq->mute = false;

    seq->first = 0;
    seq->last = seq->nsteps - 1;

    sq_sequence_noti_init(&seq->noti);
    seq->noti_enable = false;

    seq->ridx_off = 0;
    _sequence_reset_now(seq);

    return seq;

}

void sq_sequence_noti_init(sq_sequence_noti_t *noti) {

    noti->playhead_new = false;
    noti->playhead = 0;

    noti->first_new = false;
    noti->first = 0;

    noti->last_new = false;
    noti->last = 0;

    noti->transpose_new = false;
    noti->transpose = 0;

    noti->clockdivide_new = false;
    noti->clockdivide = 0;

    noti->mute_new = false;
    noti->mute = 0;


}

void _sequence_reset_now(sq_sequence_t *seq) {

    seq->idiv = 0;
    seq->tick = seq->first * seq->tps;

    if (seq->noti_enable) {
        seq->noti.playhead = seq->first;
        seq->noti.playhead_new = true;
    }

}

void _sequence_tick(sq_sequence_t *seq, jack_nframes_t idx) {

    /* this should called once per sequence, every time the session frame
        counter crosses a tick boundary */

    unsigned char *midi_msg_write_ptr;
    float dice;
    sq_trigger_t *trig;
    int widx_off;

    // serve any control messages in the ringbuffer
    _sequence_serve_ctrl_msgs(seq);

    if (seq->idiv == 0) { // clock divide

        // handle any triggers from the microgrid
        if (!seq->mute && seq->outport) {

            if ((trig = seq->microgrid[seq->tick])) { // if non-NULL

                dice = ((float) random()) / RAND_MAX;
                if (dice <= trig->probability) {

                    midi_msg_write_ptr = jack_midi_event_reserve(seq->outport->buf, idx, 3);
                    if (trig->type == TRIG_NOTE) {

                        // note on
                        midi_msg_write_ptr[0] = 143 + trig->channel;
                        midi_msg_write_ptr[1] = trig->note + seq->transpose;
                        midi_msg_write_ptr[2] = trig->velocity;

                        // note off
                        widx_off = (seq->ridx_off + (int)roundl(trig->length * seq->tps)) % seq->nticks;
                        seq->buf_off[widx_off][0] = 127 + trig->channel;
                        seq->buf_off[widx_off][1] = trig->note + seq->transpose;
                        seq->buf_off[widx_off][2] = trig->velocity;

                    } else if (trig->type == TRIG_CC) {

                        // control change (CC)
                        midi_msg_write_ptr[0] = 175 + trig->channel;
                        midi_msg_write_ptr[1] = trig->cc_number;
                        midi_msg_write_ptr[2] = trig->cc_value;

                    }

                }

            }

        }

        // increment tick index
        if (++(seq->tick) == seq->nticks) {
            seq->tick = 0;
        }

        // if we've crossed a step boundary..
        if ((seq->tick % seq->tps) == 0) {

            // handle first/last
            int step = seq->tick / seq->tps;
            if (((seq->last+1) % seq->nsteps) == step) {
                step = seq->first;
                seq->tick = step * seq->tps;
            }

            // and send a notification
            if (seq->noti_enable) {
                seq->noti.playhead = step;
                seq->noti.playhead_new = true;
            }

        }

    }

    if (++seq->idiv >= seq->div) seq->idiv = 0; // clock divide

}

void _sequence_serve_off_buffer(sq_sequence_t *seq, jack_nframes_t idx) {

    unsigned char *midi_msg_write_ptr;

    // handle any events in buf_off
    if (seq->buf_off[seq->ridx_off][0]) {  // if status byte != 0
        if (seq->outport->buf) {
            midi_msg_write_ptr = jack_midi_event_reserve(seq->outport->buf, idx, 3);
            memcpy(midi_msg_write_ptr, seq->buf_off[seq->ridx_off], 3);
            seq->buf_off[seq->ridx_off][0] = 0;  // clear this event
        }
    }

    // increment ridx_off
    if (++(seq->ridx_off) == seq->nticks) {
        seq->ridx_off = 0;
    }

}

void sq_sequence_set_name(sq_sequence_t *seq, const char *name) {

    // this parameter is safe to touch directly (for now)

    if (strlen(name) <= MAX_SEQ_NAME_LEN) {
        strcpy(seq->name, name);
    } else {
        strncpy(seq->name, name, MAX_SEQ_NAME_LEN);
        seq->name[MAX_SEQ_NAME_LEN] = '\0';
    }

}

void sq_sequence_set_outport(sq_sequence_t *seq, sq_outport_t *outport) {

    seq->outport = outport;

}

void sq_sequence_set_trig(sq_sequence_t *seq, int step_index, sq_trigger_t *trig) {

    if (seq->is_playing) {

        _sequence_ctrl_msg_t msg;
        msg.param = SEQUENCE_SET_TRIG;
        msg.vi = step_index;
        msg.vp = trig;

        _sequence_ringbuffer_write(seq, &msg);

    } else {

        _sequence_set_trig_now(seq, step_index, trig);

    }

}

void _sequence_set_trig_now(sq_sequence_t *seq, int step_index, sq_trigger_t *trig) {

    if ( (step_index < 0) || (step_index >= seq->nsteps) ) {
        fprintf(stderr, "step index %d out of range\n", step_index);
        return;
    }

    int tick_index_old, tick_index_new;

    // clear the old index
    tick_index_old = _get_tick_index_trig(seq, step_index, seq->trigs + step_index);
    seq->microgrid[tick_index_old] = NULL;

    memcpy(seq->trigs + step_index, trig, sizeof(sq_trigger_t));

    if (trig->type != TRIG_NULL) {
        // write the new index
        tick_index_new = _get_tick_index_trig(seq, step_index, trig);
        seq->microgrid[tick_index_new] = seq->trigs + step_index;
    }

}


void sq_sequence_clear_trig(sq_sequence_t *seq, int step_index) {

    if (seq->is_playing) {

        _sequence_ctrl_msg_t msg;
        msg.param = SEQUENCE_SET_TRIG;
        msg.vi = step_index;

        _sequence_ringbuffer_write(seq, &msg);

    } else {

        _sequence_clear_trig_now(seq, step_index);

    }

}

void _sequence_clear_trig_now(sq_sequence_t *seq, int step_index) {

    if ( (step_index < 0) || (step_index >= seq->nsteps) ) {
        fprintf(stderr, "step index %d out of range\n", step_index);
        return;
    }

    sq_trigger_t *trig = seq->trigs + step_index;
    seq->microgrid[_get_tick_index_trig(seq, step_index, trig)] = NULL;
    trig->type = TRIG_NULL;

}

void sq_sequence_set_transpose(sq_sequence_t *seq, int transpose) {

    if (seq->is_playing) {

        _sequence_ctrl_msg_t msg;
        msg.param = SEQUENCE_TRANSPOSE;
        msg.vi = transpose;

        _sequence_ringbuffer_write(seq, &msg);

    } else {

        _sequence_set_transpose_now(seq, transpose);

    }

}

void _sequence_set_transpose_now(sq_sequence_t *seq, int transpose) {

    seq->transpose = transpose;

    if (seq->noti_enable) {
        seq->noti.transpose = transpose;
        seq->noti.transpose_new = true;
    }

}

void sq_sequence_set_playhead(sq_sequence_t *seq, int ph) {

    if (seq->is_playing) {

        _sequence_ctrl_msg_t msg;
        msg.param = SEQUENCE_PH;
        msg.vi = ph;

        _sequence_ringbuffer_write(seq, &msg);

    } else {

        _sequence_set_playhead_now(seq, ph);

    }

}

void _sequence_set_playhead_now(sq_sequence_t *seq, int ph) {

    if ( (ph < 0) || (ph >= seq->nticks) ) {
        fprintf(stderr, "playhead value out of range: %d\n", ph);
        return;
    }

    int stick = seq->tick % seq->tps;
    seq->tick = ph * seq->tps + stick;

    if (seq->noti_enable) {
        seq->noti.playhead = ph;
        seq->noti.playhead_new = true;
    }

}

void sq_sequence_set_first(sq_sequence_t *seq, int first) {

    if (seq->is_playing) {

        _sequence_ctrl_msg_t msg;
        msg.param = SEQUENCE_FIRST;
        msg.vi = first;

        _sequence_ringbuffer_write(seq, &msg);

    } else {

        _sequence_set_first_now(seq, first);

    }

}

void _sequence_set_first_now(sq_sequence_t *seq, int first) {

    if ( (first < 0) || (first >= seq->nticks) ) {
        fprintf(stderr, "first value out of range: %d\n", first);
        return;
    }

    seq->first = first;

    if (seq->noti_enable) {
        seq->noti.first = first;
        seq->noti.first_new = true;
    }


}

void sq_sequence_set_last(sq_sequence_t *seq, int last) {

    if (seq->is_playing) {

        _sequence_ctrl_msg_t msg;
        msg.param = SEQUENCE_LAST;
        msg.vi = last;

        _sequence_ringbuffer_write(seq, &msg);

    } else {

        _sequence_set_last_now(seq, last);

    }

}

void _sequence_set_last_now(sq_sequence_t *seq, int last) {

    if ( (last < 0) || (last >= seq->nticks) ) {
        fprintf(stderr, "last value out of range: %d\n", last);
        return;
    }

    seq->last = last;

    if (seq->noti_enable) {
        seq->noti.last = last;
        seq->noti.last_new = true;
    }

}

void sq_sequence_set_clockdivide(sq_sequence_t *seq, int div) {

    if (seq->is_playing) {

        _sequence_ctrl_msg_t msg;
        msg.param = SEQUENCE_DIV;
        msg.vi = div;

        _sequence_ringbuffer_write(seq, &msg);

    } else {

        _sequence_set_clockdivide_now(seq, div);

    }

}

void _sequence_set_clockdivide_now(sq_sequence_t *seq, int div) {

    if (div < 1) {
        fprintf(stderr, "clock divide of %d is out of range (must be >= 1)\n", div);
        return;
    }

    seq->div = div;
    seq->idiv = 0;

    if (seq->noti_enable) {
        seq->noti.clockdivide = div;
        seq->noti.clockdivide_new = true;
    }

}

void sq_sequence_set_mute(sq_sequence_t *seq, bool mute) {

    if (seq->is_playing) {

        _sequence_ctrl_msg_t msg;
        msg.param = SEQUENCE_MUTE;
        msg.vb = mute;

        _sequence_ringbuffer_write(seq, &msg);

    } else {

        _sequence_set_mute_now(seq, mute);

    }

}

void _sequence_set_mute_now(sq_sequence_t *seq, bool mute) {

    seq->mute = mute;

    if (seq->noti_enable) {
        seq->noti.mute = mute;
        seq->noti.mute_new = true;
    }

}

void sq_sequence_pprint(sq_sequence_t *seq) {

    if (seq->is_playing) {

        printf("Cannot print sequence while playing.\nTry stopping the session first.\n");

    } else {

        int charCount = 0;
        bool newLineLast = false;

        for (int step = 0; step < seq->nsteps; step++) {

            if (charCount == 0) {
                printf("|");
                charCount += 1;
            }

            if (seq->trigs[step].type == TRIG_NULL) {
                printf("....|");
                charCount += 5;
                newLineLast = false;
            } else if (seq->trigs[step].type == TRIG_NOTE) {
                printf("N%03d|", seq->trigs[step].note);
                charCount += 5;
                newLineLast = false;
            } else if (seq->trigs[step].type == TRIG_CC) {
                printf("C%03d|", seq->trigs[step].cc_value);
                charCount += 5;
                newLineLast = false;
            }

            if (charCount == 81) {
                printf("\n");
                newLineLast = true;
                charCount = 0;
            }

        }

        if (!newLineLast) printf("\n");

    }

}

void sq_sequence_set_notifications(sq_sequence_t *seq, bool enable) {

    seq->noti_enable = enable;

}

bool sq_sequence_read_new_playhead(sq_sequence_t *seq, int *val) {

    bool new;

    if ((new = seq->noti.playhead_new)) {
        *val = seq->noti.playhead;
        seq->noti.playhead_new = false;
    }

    return new;

}

bool sq_sequence_read_new_first(sq_sequence_t *seq, int *val) {

    bool new;

    if ((new = seq->noti.first_new)) {
        *val = seq->noti.first;
        seq->noti.first_new = false;
    }

    return new;

}

bool sq_sequence_read_new_last(sq_sequence_t *seq, int *val) {

    bool new;

    if ((new = seq->noti.last_new)) {
        *val = seq->noti.last;
        seq->noti.last_new = false;
    }

    return new;

}

bool sq_sequence_read_new_transpose(sq_sequence_t *seq, int *val) {

    bool new;

    if ((new = seq->noti.transpose_new)) {
        *val = seq->noti.transpose;
        seq->noti.transpose_new = false;
    }

    return new;
}

bool sq_sequence_read_new_clockdivide(sq_sequence_t *seq, int *val) {

    bool new;

    if ((new = seq->noti.clockdivide_new)) {
        *val = seq->noti.clockdivide;
        seq->noti.clockdivide_new = false;
    }

    return new;
}

bool sq_sequence_read_new_mute(sq_sequence_t *seq, bool *val) {

    bool new;

    if ((new = seq->noti.mute_new)) {
        *val = seq->noti.mute;
        seq->noti.mute_new = false;
    }

    return new;
}

// read-only getters don't need to use ringbuffers

int sq_sequence_get_nsteps(sq_sequence_t *seq) {

    return seq->nsteps;

}

int sq_sequence_get_tps(sq_sequence_t *seq) {

    return seq->tps;

}

bool sq_sequence_get_mute(sq_sequence_t *seq) {

    return seq->mute;

}

int sq_sequence_get_transpose(sq_sequence_t *seq) {

    return seq->transpose;

}

int sq_sequence_get_clockdivide(sq_sequence_t *seq) {

    return seq->div;

}

int sq_sequence_get_first(sq_sequence_t *seq) {

    return seq->first;

}

int sq_sequence_get_last(sq_sequence_t *seq) {

    return seq->last;

}

////

json_object *sq_sequence_get_json(sq_sequence_t *seq) {

    json_object *jo_sequence = json_object_new_object();

    json_object_object_add(jo_sequence, "name",
                            json_object_new_string(seq->name));

    json_object_object_add(jo_sequence, "nsteps",
                            json_object_new_int(sq_sequence_get_nsteps(seq)));

    json_object_object_add(jo_sequence, "tps",
                            json_object_new_int(sq_sequence_get_tps(seq)));

    json_object_object_add(jo_sequence, "mute",
                            json_object_new_boolean(sq_sequence_get_mute(seq)));

    json_object_object_add(jo_sequence, "transpose",
                            json_object_new_int(sq_sequence_get_transpose(seq)));

    json_object_object_add(jo_sequence, "clockdivide",
                            json_object_new_int(sq_sequence_get_clockdivide(seq)));

    json_object_object_add(jo_sequence, "first",
                            json_object_new_int(sq_sequence_get_first(seq)));

    json_object_object_add(jo_sequence, "last",
                            json_object_new_int(sq_sequence_get_last(seq)));

    json_object *trigger_array = json_object_new_array();
    for (int i=0; i<seq->nsteps; i++) {
        json_object_array_add(trigger_array, sq_trigger_get_json(seq->trigs + i));
    }
    json_object_object_add(jo_sequence, "triggers", trigger_array);

    if (seq->outport) {
        json_object_object_add(jo_sequence, "outport",
                                json_object_new_string(seq->outport->name));
    } else {
        json_object_object_add(jo_sequence, "outport", NULL);
    }

    return jo_sequence;

}

sq_sequence_t *sq_sequence_malloc_from_json(json_object *jo_seq) {

    struct json_object *jo_tmp, *jo_trig;
    const char *name;
    int nsteps, tps, transpose, clockdivide, first, last;
    bool mute;
    sq_sequence_t *seq;
    sq_trigger_t trig;

    // first extract the top-level attributes

    json_object_object_get_ex(jo_seq, "name", &jo_tmp);
    name = json_object_get_string(jo_tmp);

    json_object_object_get_ex(jo_seq, "nsteps", &jo_tmp);
    nsteps = json_object_get_int(jo_tmp);

    json_object_object_get_ex(jo_seq, "tps", &jo_tmp);
    tps = json_object_get_int(jo_tmp);

    json_object_object_get_ex(jo_seq, "mute", &jo_tmp);
    mute = json_object_get_boolean(jo_tmp);

    json_object_object_get_ex(jo_seq, "transpose", &jo_tmp);
    transpose = json_object_get_int(jo_tmp);

    json_object_object_get_ex(jo_seq, "clockdivide", &jo_tmp);
    clockdivide = json_object_get_int(jo_tmp);

    json_object_object_get_ex(jo_seq, "first", &jo_tmp);
    first = json_object_get_int(jo_tmp);

    json_object_object_get_ex(jo_seq, "last", &jo_tmp);
    last = json_object_get_int(jo_tmp);

    // malloc and init the sequence

    seq = sq_sequence_new(nsteps, tps);
    sq_sequence_set_name(seq, name);
    sq_sequence_set_mute(seq, mute);
    sq_sequence_set_transpose(seq, transpose);
    sq_sequence_set_clockdivide(seq, clockdivide);
    sq_sequence_set_first(seq, first);
    sq_sequence_set_last(seq, last);

    // then set the triggers
    json_object_object_get_ex(jo_seq, "triggers", &jo_tmp);
    for (int i=0; i<nsteps; i++) {
        jo_trig = json_object_array_get_idx(jo_tmp, i);
        // trigs get copied into seqs, so we don't need to malloc them
        sq_trigger_from_json(jo_trig, &trig);
        sq_sequence_set_trig(seq, i, &trig);
    }

    return seq;

}

void sq_sequence_delete(sq_sequence_t *seq) {

    free(seq->trigs);
    free(seq->microgrid);
    free(seq->buf_off);
    free(seq);

}

