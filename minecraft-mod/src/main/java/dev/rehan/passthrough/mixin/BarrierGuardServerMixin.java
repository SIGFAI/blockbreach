package dev.rehan.passthrough.mixin;

import dev.rehan.passthrough.Passthrough;
import net.minecraft.core.BlockPos;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayerGameMode;
import net.minecraft.world.level.block.Blocks;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * The barriers are the host's floors and walls (what Steve's blocks stand on and mobs walk): a player never breaks
 * them, not even in creative (one click on one used to leave a hole into the void for the rest of the mission).
 * The server refuses here; the client doesn't even start (BarrierGuardClientMixin). No player gets barrier items
 * (operator items are off), so every barrier is the host's.
 */
@Mixin(ServerPlayerGameMode.class)
abstract class BarrierGuardServerMixin {
	@Shadow
	protected ServerLevel level;

	@Inject(method = "destroyBlock(Lnet/minecraft/core/BlockPos;)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$keepBarriers(final BlockPos pos, final CallbackInfoReturnable<Boolean> cir) {
		if (this.level.getBlockState(pos).is(Blocks.BARRIER)) {
			Passthrough.LOG.info("kept a host barrier at {} (players can't break them)", pos.toShortString());
			cir.setReturnValue(false);
		}
	}
}
