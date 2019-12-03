import os, datetime, sys, urlparse, threading, time
import SimpleHTTPServer, BaseHTTPServer
import wave
from Queue import Queue

from pygame import mixer

PORT = 8000
HOST = '0.0.0.0'

playback_queue = Queue()

class Handler(SimpleHTTPServer.SimpleHTTPRequestHandler):
    def _set_headers(self, length):
        self.send_response(200)
        if length > 0:
            self.send_header('Content-length', str(length))
        self.end_headers()

    def _get_chunk_size(self):
        data = self.rfile.read(2)
        while data[-2:] != b"\r\n":
            data += self.rfile.read(1)
        return int(data[:-2], 16)

    def _get_chunk_data(self, chunk_size):
        data = self.rfile.read(chunk_size)
        self.rfile.read(2)
        return data

    def _write_wav(self, data, rates, bits, ch):
        t = datetime.datetime.utcnow()
        time = t.strftime('%Y%m%dT%H%M%SZ')
        filename = str.format('{}_{}_{}_{}.wav', time, rates, bits, ch)

        wavfile = wave.open(filename, 'wb')
        wavfile.setparams((ch, bits/8, rates, 0, 'NONE', 'NONE'))
        wavfile.writeframes(bytearray(data))
        wavfile.close()

        return filename

    def do_GET(self):
        self.send_response(403, 'forbidden')
        return

    def do_POST(self):
        urlparts = urlparse.urlparse(self.path)
        request_file_path = urlparts.path.strip('/')
        total_bytes = 0
        sample_rates = 0
        bits = 0
        channel = 0
        if (request_file_path == 'upload'
            and self.headers.get('Transfer-Encoding', '').lower() == 'chunked'):
            data = []
            sample_rates = self.headers.get('x-audio-sample-rates', '').lower()
            bits = self.headers.get('x-audio-bits', '').lower()
            channel = self.headers.get('x-audio-channel', '').lower()
            sample_rates = self.headers.get('x-audio-sample-rates', '').lower()

            print("Audio information, sample rates: {}, bits: {}, channel(s): {}".format(sample_rates, bits, channel))
            # https://stackoverflow.com/questions/24500752/how-can-i-read-exactly-one-response-chunk-with-pythons-http-client
            while True:
                chunk_size = self._get_chunk_size()
                total_bytes += chunk_size
                print("Total bytes received: {}".format(total_bytes))
                sys.stdout.write("\033[F")
                if (chunk_size == 0):
                    break
                else:
                    chunk_data = self._get_chunk_data(chunk_size)
                    data += chunk_data

            filename = self._write_wav(data, int(sample_rates), int(bits), int(channel))

            # Queue written file for playback.
            playback_queue.put(filename)

            body = 'File {} was written, size {}'.format(filename, total_bytes)
            self._set_headers(len(body))
            self.wfile.write(body)
            self.wfile.close()
        else:
            # Deny incorrect usage.
            self.send_response(401, 'request not allowed')
            return


class Player(threading.Thread):
    def run(self):
        mixer.init(frequency=16000)
        print('Audio server started')

        while True:
            audio_fp = playback_queue.get()
            print('Received next file to play:', audio_fp)

            while mixer.music.get_busy():
                print('Already playing. Wait for 1s. More in queue:', playback_queue.unfinished_tasks)
                time.sleep(1)

            mixer.music.load(audio_fp)
            print('Playing', audio_fp)
            mixer.music.play()

            playback_queue.task_done()


httpd = BaseHTTPServer.HTTPServer((HOST, PORT), Handler)
audio_player = Player()
audio_player.start()

print("Serving HTTP on {} port {}".format(HOST, PORT));
httpd.serve_forever()

