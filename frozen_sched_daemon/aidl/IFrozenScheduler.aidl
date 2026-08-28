package android.hardware.frozensched;

interface IFrozenScheduler {
    /**
     * Freeze an app by UID with a target level.
     * level: 0=ACTIVE, 1=THROTTLED, 2=FROZEN
     */
    void freeze(int32_t uid, int32_t level);

    /**
     * Thaw an app back to ACTIVE.
     */
    void thaw(int32_t uid);

    /**
     * Query current state of an app.
     * returns: 0=ACTIVE, 1=THROTTLED, 2=FROZEN, -1=UNKNOWN
     */
    int32_t getState(int32_t uid);

    /**
     * Dump internal Lyapunov Q, per-app states, RD values (for CTS/Shell).
     */
    string dump();

    /**
     * Update RD predictor model (v1.1+ federated learning).
     * grad: serialized TFLite Micro gradient buffer.
     */
    void updateModel(byte[] grad);
}