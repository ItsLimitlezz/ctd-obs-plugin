/*
 * Spike S2 evidence: what an upload through FFmpeg's http protocol (the avio option) reports when the server refuses
 * it. Sends 8 MiB with PUT to test/s2/server.mjs: with a wrong token (401 before the body) or to "...?fail=end" (500
 * after the whole body, as a full disk or a checksum mismatch would).
 *
 *   avio-put URL TOKEN
 *
 * FFmpeg's http writer never reads the reply to a POST/PUT (http.c "pretends that it did work", and http_shutdown
 * only drains the socket without parsing it), so every call below returns success.
 */

#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: avio-put URL TOKEN\n");
		return 2;
	}
	char headers[512];
	snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", argv[2]);
	AVDictionary *options = NULL;
	av_dict_set(&options, "method", "PUT", 0);
	av_dict_set(&options, "headers", headers, 0);
	AVIOContext *io = NULL;
	int err = avio_open2(&io, argv[1], AVIO_FLAG_WRITE, NULL, &options);
	av_dict_free(&options);
	printf("avio_open2: %d (%s)\n", err, err < 0 ? av_err2str(err) : "ok");
	if (err < 0)
		return 1;
	static unsigned char block[1 << 20];
	for (int i = 0; i < 8; i++)
		avio_write(io, block, sizeof(block));
	avio_flush(io);
	printf("avio_write x8 MiB, then io->error: %d\n", io->error);
	err = avio_closep(&io);
	printf("avio_closep: %d (%s)\n", err, err < 0 ? av_err2str(err) : "ok");
	printf("=> no HTTP status reaches the caller: the server's 401/500 is lost\n");
	return 0;
}
