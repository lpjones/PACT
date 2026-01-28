print("resnet_train_tf")

import tensorflow as tf
import time

print("starting up")

# Force CPU
tf.config.set_visible_devices([], 'GPU')

# Optional: limit threads for memory stability
tf.config.threading.set_intra_op_parallelism_threads(1)
tf.config.threading.set_inter_op_parallelism_threads(1)


# Model
model = tf.keras.applications.ResNet50(
    weights=None,
    classes=1000,
    input_shape=(224, 224, 3)
)

# Synthetic data (fixed shape, persistent)
batch_size = 128
dummy_input = tf.random.normal((batch_size, 224, 224, 3))
dummy_target = tf.random.uniform(
    (batch_size,),
    minval=0,
    maxval=1000,
    dtype=tf.int32
)

# Loss & optimizer
loss_fn = tf.keras.losses.SparseCategoricalCrossentropy(from_logits=True)
optimizer = tf.keras.optimizers.SGD(learning_rate=0.01)

# One compiled training step
@tf.function(jit_compile=True)
def train_step(x, y):
    with tf.GradientTape() as tape:
        logits = model(x, training=True)
        loss = loss_fn(y, logits)
    grads = tape.gradient(loss, model.trainable_variables)
    optimizer.apply_gradients(zip(grads, model.trainable_variables))
    return loss

# Config
epochs = 2
iters_per_epoch = 2

# Warm-up (forces graph tracing + allocation ONCE)
_ = train_step(dummy_input, dummy_target)

for epoch in range(1, epochs + 1):
    start = time.time()
    for i in range(iters_per_epoch):
        print(f"Iteration {i + 1}/{iters_per_epoch}")
        loss = train_step(dummy_input, dummy_target)
    end = time.time()
    images_per_sec = (iters_per_epoch * batch_size) / (end - start)
    print(f"Epoch {epoch}: {images_per_sec:.2f} images/sec")
