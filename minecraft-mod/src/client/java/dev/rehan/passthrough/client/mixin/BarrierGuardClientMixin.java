package dev.rehan.passthrough.client.mixin;

import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.MultiPlayerGameMode;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.level.block.Blocks;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/** The host's barriers can't be broken (see BarrierGuardServerMixin): no breaking starts, no prediction removes one. */
@Mixin(MultiPlayerGameMode.class)
abstract class BarrierGuardClientMixin {
	private static boolean passthrough$barrier(final BlockPos pos) {
		Minecraft minecraft = Minecraft.getInstance();
		return minecraft.level != null && minecraft.level.getBlockState(pos).is(Blocks.BARRIER);
	}

	@Inject(method = "startDestroyBlock(Lnet/minecraft/core/BlockPos;Lnet/minecraft/core/Direction;)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$noStart(final BlockPos pos, final Direction direction, final CallbackInfoReturnable<Boolean> cir) {
		if (passthrough$barrier(pos)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(method = "continueDestroyBlock(Lnet/minecraft/core/BlockPos;Lnet/minecraft/core/Direction;)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$noContinue(final BlockPos pos, final Direction direction, final CallbackInfoReturnable<Boolean> cir) {
		if (passthrough$barrier(pos)) {
			cir.setReturnValue(false);
		}
	}

	@Inject(method = "destroyBlock(Lnet/minecraft/core/BlockPos;)Z", at = @At("HEAD"), cancellable = true)
	private void passthrough$noDestroy(final BlockPos pos, final CallbackInfoReturnable<Boolean> cir) {
		if (passthrough$barrier(pos)) {
			cir.setReturnValue(false);
		}
	}
}
