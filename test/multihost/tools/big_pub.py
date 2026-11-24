"""Publish a 200 KiB std_msgs/UInt8MultiArray on /big at 10 Hz."""
import array

import rclpy
from std_msgs.msg import UInt8MultiArray

rclpy.init()
node = rclpy.create_node("big_publisher")
pub = node.create_publisher(UInt8MultiArray, "/big", 10)
msg = UInt8MultiArray()
msg.data = array.array("B", bytes(200 * 1024))
node.create_timer(0.1, lambda: pub.publish(msg))
try:
    rclpy.spin(node)
except (KeyboardInterrupt, rclpy.executors.ExternalShutdownException):
    pass
