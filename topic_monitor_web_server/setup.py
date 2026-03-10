from setuptools import setup
from glob import glob

package_name = "topic_monitor_web_server"

setup(
    name=package_name,
    version="1.0.0",
    packages=[package_name],
    data_files=[
        ("share/ament_index/resource_index/packages", ["resource/" + package_name]),
        ("share/" + package_name, ["package.xml"]),
        ("share/" + package_name + "/static", glob("static/*")),
    ],
    install_requires=["setuptools"],
    tests_require=["pytest"],
    zip_safe=True,
    maintainer="example",
    maintainer_email="example@example.com",
    description="ROS 2 web server for browser-based topic monitor UI.",
    license="Apache-2.0",
    entry_points={
        "console_scripts": [
            "topic_monitor_web_server = topic_monitor_web_server.web_server:main",
        ],
    },
)
