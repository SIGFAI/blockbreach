package dev.rehan.passthrough.client.mixin;

import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.HitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/** Pick block (middle click) on a host barrier would hand Steve invisible barrier blocks: it picks nothing there. */
@Mixin(Minecraft.class)
abstract class PickBlockMixin {
	@Shadow
	public HitResult hitResult;

	@Shadow
	public ClientLevel level;

	@Inject(method = "pickBlockOrEntity()V", at = @At("HEAD"), cancellable = true)
	private void passthrough$noBarrierPick(final CallbackInfo ci) {
		if (this.level != null && this.hitResult instanceof BlockHitResult block && this.level.getBlockState(block.getBlockPos()).is(Blocks.BARRIER)) {
			ci.cancel();
		}
	}
}
