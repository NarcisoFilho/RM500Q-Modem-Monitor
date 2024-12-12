import rclpy
from rclpy.node import Node
from geometry_msgs.msg import PoseWithCovarianceStamped, Twist
from nav_msgs.msg import Path
import csv
import time


class PositionLogger(Node):
    def __init__(self):
        super().__init__('position_logger')

        # Subscriber for /amcl_pose
        self.amcl_subscription = self.create_subscription(
            PoseWithCovarianceStamped,
            '/amcl_pose',
            self.amcl_pose_callback,
            10
        )

        # Subscriber for /plan
        self.plan_subscription = self.create_subscription(
            Path,
            '/plan',
            self.plan_callback,
            10
        )

        # Subscriber for /cmd_vel
        self.cmd_vel_subscription = self.create_subscription(
            Twist,
            '/cmd_vel',
            self.cmd_vel_callback,
            10
        )

        # Generate the CSV filename with a timestamp
        timestamp = time.strftime('%Y%m%d_%H%M%S', time.localtime())
        filename = f'position_log_{timestamp}.csv'

        # Open the CSV file with the dynamic filename
        try:
            self.csv_file = open(filename, 'a', newline='')
            self.csv_writer = csv.writer(self.csv_file)
            # Write the header row
            self.csv_writer.writerow(['timestamp', 'source', 'x', 'y', 'z', 'linear_velocity', 'angular_velocity'])
            self.get_logger().info(f'Starting to log data to {filename}')
        except Exception as e:
            self.get_logger().error(f'Failed to open CSV file: {e}')

        self.last_logged_time_amcl = 0
        self.last_logged_time_plan = 0
        self.last_logged_time_cmd_vel = 0

    def amcl_pose_callback(self, msg):
        current_time = time.time()

        # Log only once per second for AMCL
        if current_time - self.last_logged_time_amcl >= 1.0:
            position = msg.pose.pose.position
            timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(current_time))

            # Log the absolute position in the map frame
            self.get_logger().info(f'AMCL Position - x: {position.x}, y: {position.y}, z: {position.z}')

            # Write to the CSV file
            self.csv_writer.writerow([timestamp, 'amcl_pose', position.x, position.y, position.z, None, None])
            self.csv_file.flush()  # Ensure data is immediately written to the file
            self.get_logger().info(f'Logged AMCL data: {timestamp}, x: {position.x}, y: {position.y}, z: {position.z}')

            # Update the last logged time
            self.last_logged_time_amcl = current_time

    def plan_callback(self, msg):
        current_time = time.time()

        # Log only once per second for Plan
        if current_time - self.last_logged_time_plan >= 1.0:
            timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(current_time))

            # Iterate over poses in the plan
            for pose in msg.poses:
                position = pose.pose.position

                # Log each position in the plan
                self.get_logger().info(f'Plan Pose - x: {position.x}, y: {position.y}, z: {position.z}')
                self.csv_writer.writerow([timestamp, 'plan', position.x, position.y, position.z, None, None])

            self.csv_file.flush()  # Ensure data is immediately written to the file
            self.get_logger().info(f'Logged Plan data at {timestamp} with {len(msg.poses)} poses.')

            # Update the last logged time
            self.last_logged_time_plan = current_time

    def cmd_vel_callback(self, msg):
        current_time = time.time()

        # Log only once per second for cmd_vel
        if current_time - self.last_logged_time_cmd_vel >= 1.0:
            linear = msg.linear
            angular = msg.angular
            timestamp = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(current_time))

            # Log the linear and angular velocity
            self.get_logger().info(f'Cmd Vel - linear: {linear.x}, angular: {angular.z}')

            # Write to the CSV file
            self.csv_writer.writerow([timestamp, 'cmd_vel', None, None, None, linear.x, angular.z])
            self.csv_file.flush()  # Ensure data is immediately written to the file
            self.get_logger().info(f'Logged Cmd Vel data: {timestamp}, linear: {linear.x}, angular: {angular.z}')

            # Update the last logged time
            self.last_logged_time_cmd_vel = current_time

    def destroy_node(self):
        # Close the CSV file before the node is destroyed
        try:
            self.csv_file.close()
            self.get_logger().info('Stopping logging and closing the CSV file.')
        except Exception as e:
            self.get_logger().error(f'Failed to close CSV file: {e}')
        super().destroy_node()


def main(args=None):
    rclpy.init(args=args)

    position_logger = PositionLogger()

    try:
        rclpy.spin(position_logger)
    except KeyboardInterrupt:
        # Log message before calling rclpy.shutdown()
        if rclpy.ok():
            position_logger.get_logger().info('Keyboard Interrupt (Ctrl+C) detected, shutting down.')
    finally:
        if rclpy.ok():  # Check if the context is still active
            position_logger.destroy_node()
            rclpy.shutdown()


if __name__ == '__main__':
    main()

