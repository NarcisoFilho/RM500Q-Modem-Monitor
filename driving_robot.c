#include<stdio.h>
#include<time.h>
#include<stdlib.h>
#include<unistd.h>



#define WAITING_TIME_S 10
#define LOOP_COUNT 5
#define POINTS_COUNT 11

typedef struct Point{
   float x;
   float y;
} Point;


int main(int argc, char *argv[]){
   Point point_list[POINTS_COUNT] = {
      {2, -3.62},      //9
      {17.60, 4.30}, //2
      {-17.60, -3.62},  //7
      {-9.1, 4.30},  //5
      {-17.60, 4.30},  //6
      {8.8, -3.62},      //10
      {2,0},      //11
      {2, 4.30},    //4
      {-8.8, -3.62},   //8
      {8.8, 4.30},   //3   	
      {17.60,-3.62}, //1
   };
   int loop_count = LOOP_COUNT;
   int waiting_time = WAITING_TIME_S;


   if(argc >= 2){
      loop_count = atoi(argv[1]);
   }
   if(argc >= 3){
      waiting_time = atoi(argv[2]);
   }
   if(argc >= 4){
      //  = atoi(argv[2]);
   }
   
   char command[250] = "";
   for(int i = loop_count; i ; i--){
	for(int j = 0; j < POINTS_COUNT; j++){		
		sprintf(command,"ros2 action send_goal /navigate_to_pose nav2_msgs/action/NavigateToPose --feedback \"{pose: {header: {frame_id: 'map'}, pose: {position: {x: %.3f, y: %.3f, z: 0.0}, orientation: {x: 0.0, y: 0.0, z: 0.0, w: 1.0}}}}\"", point_list[j].x, point_list[j].y);
		printf("%f ; %f\n", point_list[j].x, point_list[j].y);
      		system(command);      
      		sleep(waiting_time);
      	}
   }

   return 0;
}
