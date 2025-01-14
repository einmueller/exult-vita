/*
Copyright (C) 2004-2005  The Pentagram Team
Copyright (C) 2006-2022  The Exult Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

/*
  ALSA MIDI driver
  Adapted from ScummVM's backends/midi/alsa.cpp
  (which is in turn
   "Mostly cut'n'pasted from Virtual Tiny Keyboard (vkeybd) by Takashi Iwai" )
*/

#include "pent_include.h"
#include "ALSAMidiDriver.h"

#ifdef USE_ALSA_MIDI

const MidiDriver::MidiDriverDesc ALSAMidiDriver::desc =
		MidiDriver::MidiDriverDesc ("Alsa", createInstance);




#if SND_LIB_MAJOR >= 1 || SND_LIB_MINOR >= 6
#define snd_seq_flush_output(x) snd_seq_drain_output(x)
#define snd_seq_set_client_group(x,name)	/*nop */
#define my_snd_seq_open(seqp) snd_seq_open(seqp, "hw", SND_SEQ_OPEN_OUTPUT, 0)
#else
/* SND_SEQ_OPEN_OUT causes oops on early version of ALSA */
#define my_snd_seq_open(seqp) snd_seq_open(seqp, SND_SEQ_OPEN)
#endif

#define ALSA_PORT "65:0"
#define ADDR_DELIM ".:"


ALSAMidiDriver::ALSAMidiDriver()
 : isOpen(false), seq_handle(nullptr), seq_client(0), seq_port(0),
   my_client(0), my_port(0)
{

}

int ALSAMidiDriver::open() {
	std::string arg;
	unsigned int caps;

	if (isOpen)
		return -1;

	arg = getConfigSetting("alsa_port", ALSA_PORT);

	if (parse_addr(arg, &seq_client, &seq_port) < 0) {
		perr << "ALSAMidiDriver: Invalid port: " << arg << std::endl;
		return -1;
	}
	
	if (my_snd_seq_open(&seq_handle)) {
		perr << "ALSAMidiDriver: Can't open sequencer" << std::endl;
		return -1;
	}

	isOpen = true;
	
	my_client = snd_seq_client_id(seq_handle);
	snd_seq_set_client_name(seq_handle, "PENTAGRAM");
	snd_seq_set_client_group(seq_handle, "input");
	
	caps = SND_SEQ_PORT_CAP_READ;
	if (seq_client == SND_SEQ_ADDRESS_SUBSCRIBERS)
		caps = ~SND_SEQ_PORT_CAP_SUBS_READ;
	my_port =
		snd_seq_create_simple_port(seq_handle, "PENTAGRAM", caps,
								   SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
	if (my_port < 0) {
		snd_seq_close(seq_handle);
		isOpen = false;
		perr << "ALSAMidiDriver: Can't create port" << std::endl;
		return -1;
	}

	if (seq_client != SND_SEQ_ADDRESS_SUBSCRIBERS) {
		/* subscribe to MIDI port */
		if (snd_seq_connect_to(seq_handle, my_port, seq_client, seq_port) < 0) {
			snd_seq_close(seq_handle);
			isOpen = false;
			perr << "ALSAMidiDriver: "
				 << "Can't subscribe to MIDI port (" << seq_client
				 << ":" << seq_port << ")" << std::endl;
			return -1;
		}
	}

	pout << "ALSA client initialised [" << seq_client << ":"
		 << seq_port << "]" << std::endl;

	return 0;
}

void ALSAMidiDriver::close() {
	isOpen = false;
	if (seq_handle)
		snd_seq_close(seq_handle);
}

void ALSAMidiDriver::send(uint32 b) {
	unsigned int midiCmd[4];
	ev.type = SND_SEQ_EVENT_OSS;

	midiCmd[3] = (b & 0xFF000000) >> 24;
	midiCmd[2] = (b & 0x00FF0000) >> 16;
	midiCmd[1] = (b & 0x0000FF00) >> 8;
	midiCmd[0] = (b & 0x000000FF);
	ev.data.raw32.d[0] = midiCmd[0];
	ev.data.raw32.d[1] = midiCmd[1];
	ev.data.raw32.d[2] = midiCmd[2];

	unsigned char chanID = midiCmd[0] & 0x0F;
	switch (midiCmd[0] & 0xF0) {
	case 0x80:
		snd_seq_ev_set_noteoff(&ev, chanID, midiCmd[1], midiCmd[2]);
		send_event(1);
		break;
	case 0x90:
		snd_seq_ev_set_noteon(&ev, chanID, midiCmd[1], midiCmd[2]);
		send_event(1);
		break;
	case 0xB0:
		/* is it this simple ? Wow... */
		snd_seq_ev_set_controller(&ev, chanID, midiCmd[1], midiCmd[2]);
		send_event(1);
		break;
	case 0xC0:
		snd_seq_ev_set_pgmchange(&ev, chanID, midiCmd[1]);
		send_event(0);
		break;
	case 0xD0:
		snd_seq_ev_set_chanpress(&ev, chanID, midiCmd[1]);
		send_event(0);
		break;
	case 0xE0:{
			// long theBend = ((((long)midiCmd[1] + (long)(midiCmd[2] << 7))) - 0x2000) / 4;
			// snd_seq_ev_set_pitchbend(&ev, chanID, theBend);
			long theBend = (static_cast<long>(midiCmd[1]) + static_cast<long>(midiCmd[2] << 7)) - 0x2000;
			snd_seq_ev_set_pitchbend(&ev, chanID, theBend);
			send_event(1);
		}
		break;

	default:
		perr << "ALSAMidiDriver: Unknown Command: "
			 << std::hex << static_cast<int>(b) << std::dec << std::endl;
		/* I don't know if this works but, well... */
		send_event(1);
		break;
	}
}

void ALSAMidiDriver::send_sysex(uint8 status,const uint8 *msg,uint16 length) {
	unsigned char buf[1024];

	if (length > 511) {
		perr << "ALSAMidiDriver: "
			 << "Cannot send SysEx block - data too large" << std::endl;
		return;
	}
	buf[0] = status;
	memcpy(buf + 1, msg, length);
	snd_seq_ev_set_sysex(&ev, length + 1, &buf);
	send_event(1);
}

// static
int ALSAMidiDriver::parse_addr(const std::string& _arg, int *client, int *port) {
	const char* arg = _arg.c_str();

	if (isdigit(static_cast<unsigned char>(*arg))) {
		const char *p;
		if ((p = strpbrk(arg, ADDR_DELIM)) == nullptr)
			return -1;
		*client = atoi(arg);
		*port = atoi(p + 1);
	} else {
		if (*arg == 's' || *arg == 'S') {
			*client = SND_SEQ_ADDRESS_SUBSCRIBERS;
			*port = 0;
		} else
			return -1;
	}
	return 0;
}

void ALSAMidiDriver::send_event(int do_flush) {
	snd_seq_ev_set_direct(&ev);
	snd_seq_ev_set_source(&ev, my_port);
	snd_seq_ev_set_dest(&ev, seq_client, seq_port);

	snd_seq_event_output(seq_handle, &ev);
	if (do_flush)
		snd_seq_flush_output(seq_handle);
}

#endif
