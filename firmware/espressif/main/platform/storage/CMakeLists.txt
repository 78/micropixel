# Block storage adapters shared by every board: the NOR app_store partition
# and external media that boards may publish as their App store.
list(APPEND micropixel_sources
    "platform/storage/partition_block_storage.cpp"
)
