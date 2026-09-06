#include "mqtt.h"

#include "logging.h"

#include <mosquitto.h>

namespace mqtt {
    bool init() {
        const int rc = mosquitto_lib_init();
        if (rc != MOSQ_ERR_SUCCESS) {
            logging::error("mosquitto_lib_init falhou: {}", mosquitto_strerror(rc));
            return false;
        }

        int major = 0;
        int minor = 0;
        int revision = 0;
        mosquitto_lib_version(&major, &minor, &revision);
        logging::debug("Iotrail using libmosquitto {}.{}.{}", major, minor, revision);
        return true;
    }

    void shutdown() {
        mosquitto_lib_cleanup();
    }
}
