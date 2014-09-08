#include <arpa/inet.h>
#include <image_transport/image_transport.h>
#include <x264_image_transport/x264Packet.h>
#include <signal.h>	
#include <sys/socket.h>

/**
 * Receives ArDrone 2.0 video stream and publishes it on x264_image_transport
 */

typedef struct { //PaVE
    char signiture[4]; // "PaVE" - used to identify the start of frame
    uint8_t version; // Version code
    uint8_t video_codec; // Codec of the following frame
    uint16_t header_size; // Size of the parrot_video_encapsulation_t
    uint32_t payload_size; // Amount of data following this PaVE
    uint16_t encoded_stream_width; // ex: 640
    uint16_t encoded_stream_height; // ex: 368
    uint16_t display_width; // ex: 640
    uint16_t display_height; // ex: 360
    uint32_t frame_number; // Frame position inside the current stream
    uint32_t timestamp; // In milliseconds
    uint8_t total_chuncks; // Number of UDP packets containing the current decodable payload - currently unused
    uint8_t chunck_index; // Position of the packet - first chunk is #0 - currenty unused
    uint8_t frame_type; // I-frame, P-frame - parrot_video_encapsulation_frametypes_t
    uint8_t control; // Special commands like end-of-stream or advertised frames
    uint32_t stream_byte_position_lw; // Byte position of the current payload in the encoded stream - lower 32-bit word
    uint32_t stream_byte_position_uw; // Byte position of the current payload in the encoded stream - upper 32-bit word
    uint16_t stream_id; // This ID indentifies packets that should be recorded together
    uint8_t total_slices; // number of slices composing the current frame
    uint8_t slice_index; // position of the current slice in the frame
    uint8_t header1_size; // H.264 only : size of SPS inside payload - no SPS present if value is zero
    uint8_t header2_size; // H.264 only : size of PPS inside payload - no PPS present if value is zero
    uint8_t reserved2[2]; // Padding to align on 48 bytes
    uint32_t advertised_size; // Size of frames announced as advertised frames
    uint8_t reserved3[12]; // Padding to align on 64 bytes
    uint8_t reserved4[4]; // padding -- added b/c it was in the KIPR library code
} __attribute__ ((packed)) parrot_video_encapsulation_t;

typedef enum {
    //PaVE codec IDs
    CODEC_UNKNNOWN = 0,
    CODEC_VLIB,
    CODEC_P264,
    CODEC_MPEG4_VISUAL,
    CODEC_MPEG4_AVC
} parrot_video_encapsulation_codecs_t;

typedef enum {
	//PaVE frame types
    FRAME_TYPE_UNKNNOWN = 0, FRAME_TYPE_IDR_FRAME, // headers followed by I-frame
    FRAME_TYPE_I_FRAME, FRAME_TYPE_P_FRAME, FRAME_TYPE_HEADERS
} parrot_video_encapsulation_frametypes_t;


const int one = 1;

int establish_socket(const std::string* name, sockaddr_in* myAddr, sockaddr_in* droneAddr, const struct timeval* timeout){
	//***************************************************************************
	//  Set up sockets
	//***************************************************************************

	int socketNumber = socket(AF_INET, SOCK_STREAM, 0);
	while(ros::ok() && connect(socketNumber, (sockaddr*) droneAddr, sizeof(sockaddr_in)) != 0) {
		ROS_INFO("[%s]Did not manage to establish connection", (*name).c_str());
		ros::Duration((*timeout).tv_sec + (*timeout).tv_usec / 1000000.0).sleep();
	}
	setsockopt(socketNumber, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(socketNumber, SOL_SOCKET, SO_RCVTIMEO, (char *)timeout,sizeof(struct timeval));
	return socketNumber;
}

int fetch_video(ros::NodeHandle nh, std::string drone_ip, int drone_port,
		int buffer_size, struct timeval timeout, std::string name){
	//struct sigaction sa;
	//memset(&sa, 0, sizeof(sa));
	//sa.sa_handler = quit_signal;
	//sigfillset(&sa.sa_mask);
	//sigaction(SIGINT,&sa,NULL);
	//***************************************************************************
	//   Socket Addresses
	//***************************************************************************
	sockaddr_in myAddr;
	sockaddr_in droneAddr;
	bzero(&myAddr, sizeof(myAddr));
	bzero(&droneAddr, sizeof(droneAddr));
	myAddr.sin_family = AF_INET;
	myAddr.sin_addr.s_addr = INADDR_ANY;
	droneAddr.sin_family = AF_INET;
	droneAddr.sin_addr.s_addr = inet_addr(drone_ip.c_str());
	droneAddr.sin_port = htons(drone_port);
	//***************************************************************************
	//   Helper variables
	//***************************************************************************
	int index, read, result, i;
	const uint16_t* header_size;
	const uint32_t* payload_size;
	unsigned char part[buffer_size];
	int partLength;
	bool check;
	x264_image_transport::x264Packet message;
	parrot_video_encapsulation_t* pave;
	index = 0;
	int errorCount = 0;
	//***************************************************************************
	//   Initialise connection and publisher
	//***************************************************************************
	int socketNumber = establish_socket(&name, &myAddr, &droneAddr, &timeout);
	ros::Publisher pub = nh.advertise<x264_image_transport::x264Packet>("/" + name + "/video/x264", 1000);
	//***************************************************************************
	//   Decode PaVE packet and send the encoded video stream
	//***************************************************************************
	ROS_INFO("[%s]***** START VIDEO STREAM *****", name.c_str());
	while (ros::ok()) {
		if(index == 0) {
			partLength = TEMP_FAILURE_RETRY(recv(socketNumber, part, buffer_size,0));
			if (partLength <= 0) {
				ROS_INFO("[%s][%d]Did not receive video data, trying to recover", name.c_str(), partLength);
				if(errorCount > 5){
					close(socketNumber);
					ros::Duration(timeout.tv_sec + timeout.tv_usec / 1000000.0).sleep();
				}
				socketNumber = establish_socket(&name, &myAddr, &droneAddr, &timeout);
				errorCount++;
				index = 0;
				continue;
			}
			errorCount = 0;
		}
		pave = (parrot_video_encapsulation_t *) (part+index);
		//ROS_INFO("Codec: %d",pave->video_codec);
		if (strncmp(pave->signiture,"PaVE", 4) != 0) {
			ROS_INFO("[%s]PaVE not synchronized, trying to rebind", name.c_str());
			for(i = 0;i<buffer_size-index-3;i++)
				if(strncmp((const char*) (part+index+i),"PaVE", 4) == 0){
					index += i;
					break;
				}
			if(i == buffer_size-index-3)
				index = 0;
			continue;
		}
		header_size = (const uint16_t*) (part + index + 6);
		payload_size = (const uint32_t*) (part + index + 8);
		message.img_width = *(const uint16_t*) (part + index + 16);
		message.img_height = *(const uint16_t*) (part + index + 18);
		message.header.stamp.fromSec(*(const uint32_t*) (part + index + 24) / 1000.0);
		message.codec = pave->video_codec == 4 ? x264_image_transport::x264Packet::CODEC_H264 : 
				pave->video_codec == 3 ? x264_image_transport::x264Packet::CODEC_MPEG4 : -1;
		if(index + *header_size + *payload_size > buffer_size){
			ROS_INFO("[%s]Too big payload, skipping frame.(ADVICE: Increase buffer_size)", name.c_str());
			index = 0;
			continue;
		}
		// This packet did not contain all the data
		if(partLength - index -  *header_size < *payload_size) {
			read = partLength - index - *header_size;
			check = false;
			while (read < *payload_size) {
				partLength = TEMP_FAILURE_RETRY(recv(socketNumber, part+index+*header_size+read, *payload_size - read,0));
				if(partLength <= 0){
					check = true;
					break;
				}
				read += partLength;
			}
			if(check){
				ROS_INFO("[%s]Timedout while waiting extra packets", name.c_str());
				index = 0;
				continue;
			}		
			partLength = index + *header_size + *payload_size;
		}

		message.data.assign(part+index+*header_size, part+index+*header_size+*payload_size);
		pub.publish(message);
		//Received more than one packet in the buffer
		if(partLength - index - *header_size > *payload_size)
			index += *header_size + *payload_size;
		else
			index = 0;
	}
	ROS_INFO("[%s]Closing socket.", name.c_str());//, socketNumber, flag, ros::ok());
	close(socketNumber);
	return 0;
}

int main(int argc, char **argv)
{
	//***************************************************************************
	//	Initialise this ROS Node and read ROS parameters
	//***************************************************************************
	ros::init(argc, argv, "x264_test_publisher");
	ros::NodeHandle nh("~");
	
	int buffer_size;
	nh.param<int>("buffer_size", buffer_size, 65536);
	int drone_port;
	nh.param<int>("drone_port", drone_port, 5555);
	struct timeval timeout;
	int temp;
	nh.param<int>("timeout", temp, 1000);
	timeout.tv_sec = temp / 1000;
	timeout.tv_usec = (temp - timeout.tv_sec) * 1000;
	std::string name;
	nh.param<std::string>("name", name, "drone");
	std::string drone_ip = "";
	std::vector<std::string> active;
	if(ros::param::get("/drones_active", active)){
		bool drone = false;
		for(int i=0;i<active.size();i++)
			if(active[i].compare(name) == 0)
				drone = true;
		if(!drone)
			return 0;
		nh.getParam("/drones/" + name + "/ip", drone_ip);
	} 
	if(drone_ip.compare("") == 0){
		ROS_INFO("[%s]Did not found IP in the parameter server, switchin to args for IP and PORT", name.c_str());
		nh.param<std::string>("drone_ip", drone_ip, "192.168.1.1");	
	}
	image_transport::ImageTransport it(nh);
	return fetch_video(nh, drone_ip, drone_port, buffer_size, timeout, name);
}

