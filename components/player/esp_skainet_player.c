#include "stdio.h"
#include "stdlib.h"
#include "esp_skainet_player.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wav_decoder.h"
#include "driver/i2s.h"
#include "freertos/queue.h"
#include <sys/stat.h>
#include <sys/dirent.h>

#define CODEC_CHANNEL 2
#define CODEC_SAMPLE_RATE 16000

typedef struct
{
	QueueHandle_t player_queue;
	int rb_size;
	int frame_size;
	char **file_list;
	int file_num;
	int max_file_num;
	int player_state;
	int vol;
	TaskHandle_t stream_in;
	TaskHandle_t stream_out;
}esp_skainet_player_handle_t;


void esp_skainet_stream_in_task(void *arg)
{
	esp_skainet_player_handle_t *player=arg;
	unsigned char* buffer=malloc(player->frame_size*sizeof(unsigned char)*2);
	void * wav_decoder=NULL;
	int cur_file_num=0;
	printf("create stream in\n");
	int count=0;
	int channels=CODEC_CHANNEL;
	int sample_rate=CODEC_SAMPLE_RATE;

	while(1) {
		count++;
		switch(player->player_state) {
			case 1: // play
			    if(player->file_num<=0) {
			    	printf("playlist is empty\n");
			    	player->player_state=0;
			    } else {
			    	
			    	while (wav_decoder==NULL) {
			    		wav_decoder=wav_decoder_open(player->file_list[cur_file_num]);

			    		if (wav_decoder==NULL) {
			    			printf("can not find %s, play next song\n", player->file_list[cur_file_num]);
			    		} else if(wav_decoder_get_sample_rate(wav_decoder) != CODEC_SAMPLE_RATE) {
			    			//resample
			    			printf("The sample rate of %s does not meet the requirements, please resample to %d\n", 
			    				   player->file_list[cur_file_num], CODEC_SAMPLE_RATE);
			    			wav_decoder_close(wav_decoder);
			    			continue;
			    		} else {  
			    			channels=wav_decoder_get_channel(wav_decoder);
			    			sample_rate=wav_decoder_get_sample_rate(wav_decoder);
			    			printf("start to play %s, channels:%d, sample rate:%d \n", player->file_list[cur_file_num],
			    				channels, sample_rate );
				    		cur_file_num++;
				    		cur_file_num=cur_file_num%player->file_num;
			    		}			    		
			    	} 

			        int size=wav_decoder_run(wav_decoder, buffer, player->frame_size);

			        if (size<player->frame_size){
			        	memset(buffer+size, 0, player->frame_size-size);
			        	wav_decoder_close(wav_decoder);
			        	wav_decoder=NULL;
			        }

			        if (channels != CODEC_CHANNEL) {
			        	if (CODEC_CHANNEL==2 && channels==1){

			        		for (int i=player->frame_size-1; i>=1; i-=2) {
			        			buffer[i*2+1]=buffer[i];
			        			buffer[i*2]=buffer[i-1];
			        			buffer[i*2-1]=buffer[i];
			        			buffer[i*2-2]=buffer[i-1];
			        			//printf("%d %d %d %d  -> %d %d\n", buffer[i*2+1], buffer[i*2], buffer[i*2-1], buffer[i*2-2], buffer[i], buffer[i-1]);
			        		}

			        		//rb_write(player->player_queue, (uint8_t *)buffer, player->frame_size*2, portMAX_DELAY);
			        		xQueueSend(player->player_queue, buffer, portMAX_DELAY);
			        		xQueueSend(player->player_queue, buffer+player->frame_size, portMAX_DELAY);
			        	}
			        } else {
			        	//rb_write(player->player_queue, (uint8_t *)buffer, player->frame_size, portMAX_DELAY);
			        	xQueueSend(player->player_queue, buffer, portMAX_DELAY);
			        }

			        // rb_write(player->player_queue, (uint8_t *)buffer, player->frame_size, portMAX_DELAY);
			        //xQueueSend(player->player_queue, buffer, portMAX_DELAY);
			        
			    }
			    break;
			case 2: // pause or stop
				vTaskDelay(16 / portTICK_PERIOD_MS);
				break;

			case 3: // continue
				player->player_state=1;
				break;

			case 4: // exit
				free(buffer);
				if (wav_decoder!=NULL) 
					wav_decoder_close(wav_decoder);
				return ;

			default: // exit
			    // printf("delay\n");
				vTaskDelay(16 / portTICK_PERIOD_MS);

		}
    }
}

void esp_skainet_stream_out_task(void *arg)
{
	esp_skainet_player_handle_t *player=arg;
	unsigned char* buffer=malloc(player->frame_size*sizeof(unsigned char));
	size_t bytes_write=0;
	printf("create stream_out\n");
	int count=0;

	while(1) {
		count++;
		switch(player->player_state) {
			case 1: // play
				//rb_read(player->player_queue, (uint8_t *)buffer, player->frame_size, portMAX_DELAY);
				xQueueReceive(player->player_queue, buffer, portMAX_DELAY);
                i2s_write(0, buffer, player->frame_size, &bytes_write, portMAX_DELAY);

			    break;

			case 2: // pause or stop
			    i2s_zero_dma_buffer(0);
			    vTaskDelay(16 / portTICK_PERIOD_MS);
				break;

			case 3: // continue
				player->player_state=1;
				// vTaskDelay(16 / portTICK_PERIOD_MS);
				break;
				
			case 4: // exit
				free(buffer);
				return ;

			default: // exit
				i2s_zero_dma_buffer(0);
				vTaskDelay(16 / portTICK_PERIOD_MS);

		}
    }
}



int file_list_scan(void *handle, const char *path)
{
	esp_skainet_player_handle_t *player=handle;
    struct dirent *ret;
    DIR *dir;
    dir = opendir(path);
    int path_len=strlen(path);
    if (dir != NULL) {

        while ((ret = readdir(dir)) != NULL && player->file_num < player->max_file_num) { // NULL if reach the end of directory
        	
        	if (ret->d_type!=1) // continue if d_type is not file
        		continue;

        	int len=strlen(ret->d_name);
        	if (len > FATFS_PATH_LENGTH_MAX-path_len-1) // continue if name is too long
        		continue;

        	char *suffix=ret->d_name+len-4;

        	if (strcmp(suffix, ".wav")==0 || strcmp(suffix, ".WAV")==0 ) {

        		memset(player->file_list[player->file_num], 0, FATFS_PATH_LENGTH_MAX);
        		memcpy(player->file_list[player->file_num], path, path_len);
        	    memcpy(player->file_list[player->file_num]+path_len, ret->d_name, len+1);
        	    printf("%d -> %s\n",player->file_num, player->file_list[player->file_num]);
        	    player->file_num++;
        	}
        }
        closedir(dir);
    } else {
        printf("opendir NULL \r\n");
    }
    return player->file_num;
}

void *esp_skainet_player_create(int ringbuf_size, unsigned int core_num)
{
	if (ringbuf_size<1024)
		ringbuf_size=1024;
	else
		ringbuf_size=(ringbuf_size/1024+1)*1024;

	if (core_num>1)
		core_num=1;

	esp_skainet_player_handle_t *player=malloc(sizeof(esp_skainet_player_handle_t));

	player->frame_size=256;
	player->rb_size=ringbuf_size;
	//player->player_queue=rb_init(BUFFER_PROCESS, player->rb_size, 1, NULL);
	player->player_queue=xQueueCreate(ringbuf_size/player->frame_size, player->frame_size);
	player->player_state=0;
	player->file_num=0;
	player->max_file_num=10;
	player->file_list=malloc(sizeof(char *)*player->max_file_num);
	for (int i=0; i<player->max_file_num; i++)
		player->file_list[i]=calloc(FATFS_PATH_LENGTH_MAX, sizeof(char));

	xTaskCreatePinnedToCore(esp_skainet_stream_in_task, "stream_in", 2 * 1024, (void*)player, 5, NULL, core_num);
	xTaskCreatePinnedToCore(esp_skainet_stream_out_task, "stream_out", 2 * 1024, (void*)player, 5, NULL, core_num);

	return player;
}


void esp_skainet_player_play(void *handle, const char *path)
{
	esp_skainet_player_handle_t *player=handle;
	//create file list

	file_list_scan(player, path);
	// for (int i=0; i<player->file_num; i++)
	// 	printf("%s\n", player->file_list[i]);

	//set player state
	player->player_state=1;

}


void esp_skainet_player_pause(void *handle)
{
	esp_skainet_player_handle_t *player=handle;
	player->player_state=2;
	printf("pause\n");
}

void esp_skainet_player_continue(void *handle)
{
	esp_skainet_player_handle_t *player=handle;
	player->player_state=3;
	printf("play\n");
}

void esp_skainet_player_exit(void *handle)
{
	esp_skainet_player_handle_t *player=handle;
	player->player_state=4;
}

int esp_skainet_player_get_state(void *handle)
{
	esp_skainet_player_handle_t *player=handle;
	return player->player_state;
}

// void esp_skainet_player_increase_vol(void *handle)
// {
// 	int vol=65;
// 	MediaHalGetVolume(&vol);
// 	if (vol<50) {
// 		vol += 5;
// 	} else if (vol<60) {
// 		vol += 2;
// 	} else if (vol<75) {
// 		vol += 1;
// 	} else {
// 		vol = 75;
// 	}
// 	MediaHalSetVolume(vol);
// 	printf("vol: %d\n", vol);
// }

// void esp_skainet_player_decrease_vol(void *handle)
// {
// 	int vol=65;
// 	MediaHalGetVolume(&vol);
// 	if (vol<=50) {
// 		vol -= 5;
// 	} else if (vol<=60) {
// 		vol -= 2;
// 	} else if (vol<=75) {
// 		vol -= 1;
// 	} else {
// 		vol = 75;
// 	}
// 	MediaHalSetVolume(vol);
// 	printf("vol: %d\n", vol);
// }


